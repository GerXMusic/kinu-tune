// Arnes del adaptador de M2 (D-093, niveles 1 y 2/3).
//
// NO se distribuye con el plugin: es una herramienta de medida. Ejercita la
// MISMA traduccion (PitchcoreRequest.h) y el MISMO motor (ClassicEngine) que
// usa PitchcoreSynthesizer::render; la unica diferencia es la comprobacion de
// cancelacion, que aqui no existe.
//
// Construye la peticion como la construye el plugin (IncrementalSynthesizer):
// tramo del original con 4096 muestras de contexto, hop de 512 y curvas por
// frame LOCAL de la region. Con eso:
//
//   NIVEL 1 (D-093 §1): contrato de longitud (out == numOutputFrames*hop) y,
//     con 0 cents, la salida es el original: corr >= 0,9999 y LAG 0,00 +-0,02.
//   CONTROL: si el rms de la region < 1e-4, FALLA: el silencio "pasa"
//     cualquier comparacion y no mide nada.
//   NIVEL 3 (D-093 §3): --vs <cli.wav> compara la salida con la referencia
//     (el render del CLI) por ALINEACION FRACCIONAL: corr >= 0,9999 y
//     rms <= 1e-3, sin C1. Informa el lag, que es lo que se compara con I-7.
//   NIVEL 2 (D-093 §2): --identical <bounce_viejo.wav> exige 0 muestras
//     distintas (PSOLA byte a byte con el adaptador compilado y sin seleccionar).
//   --selftest valida el estimador de lag con un desplazamiento entero conocido.
//
// Uso:
//   pitchcore_adapter_check <wav> [cents] [startSec] [durSec]
//       [--out salida.wav] [--vs referencia.wav] [--identical otra.wav] [--selftest]

#include "PitchcoreRequest.h"
#include "pitchcore/f0.h"
#include "pitchcore/pitch_curve.h"
#include "pitchcore/psola.h"
#include "pitchcore/wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr int kHop = 512;
constexpr int kContext = 4096;

// Umbrales escritos en D-093/D-097 (no se tocan aqui).
constexpr double kCorrMin = 0.9999;
constexpr double kRmsMax = 1e-3;
constexpr double kSilenceRms = 1e-4;
constexpr double kLagTol = 0.02;

struct Grid
{
    std::vector<float> f0;
    std::vector<uint8_t> voiced;
};

Grid makeGrid(const std::vector<pitchcore::F0Frame> &frames, int totalFrames,
              double sr)
{
    Grid g;
    g.f0.assign(static_cast<size_t>(totalFrames), 0.f);
    g.voiced.assign(static_cast<size_t>(totalFrames), 0);
    if (frames.empty())
        return g;

    size_t nearest = 0;
    for (int k = 0; k < totalFrames; ++k)
    {
        const double t = (static_cast<double>(k) * kHop) / sr;
        while (nearest + 1 < frames.size() &&
               std::fabs(frames[nearest + 1].timeSec - t) <
                   std::fabs(frames[nearest].timeSec - t))
            ++nearest;
        g.f0[static_cast<size_t>(k)] = static_cast<float>(frames[nearest].f0Hz);
        g.voiced[static_cast<size_t>(k)] = frames[nearest].voiced ? 1 : 0;
    }
    return g;
}

struct LagFit
{
    double lag = 0.0;   // out[i] ~ ref[i - lag]: lag > 0 = out retrasado
    double corr = 0.0;
    double rms = 0.0;
    long n = 0;
    bool ok = false;
};

double correlationAt(const std::vector<float> &out, const std::vector<float> &ref,
                     int lag)
{
    double num = 0, xx = 0, yy = 0;
    const int n = static_cast<int>(out.size());
    for (int i = 0; i < n; ++i)
    {
        const int j = i - lag;
        if (j < 0 || j >= static_cast<int>(ref.size())) continue;
        const double a = out[static_cast<size_t>(i)];
        const double b = ref[static_cast<size_t>(j)];
        num += a * b; xx += a * a; yy += b * b;
    }
    return (xx > 0 && yy > 0) ? num / std::sqrt(xx * yy) : 0.0;
}

// Busca el lag entero que maximiza la correlacion y lo refina con una parabola
// por los tres puntos. Resolucion practica ~0,01 muestras, que es lo que hace
// falta para distinguir 0,00 de -2,52 (I-7).
LagFit measureLag(const std::vector<float> &out, const std::vector<float> &ref,
                  int maxLag = 32)
{
    LagFit fit;
    if (out.empty() || ref.empty()) return fit;

    int bestLag = 0;
    double best = -2.0;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        const double c = correlationAt(out, ref, lag);
        if (c > best) { best = c; bestLag = lag; }
    }

    double refined = static_cast<double>(bestLag);
    if (bestLag > -maxLag && bestLag < maxLag)
    {
        const double cm = correlationAt(out, ref, bestLag - 1);
        const double c0 = best;
        const double cp = correlationAt(out, ref, bestLag + 1);
        const double denom = cm - 2.0 * c0 + cp;
        if (std::fabs(denom) > 1e-12)
            refined += 0.5 * (cm - cp) / denom;
    }

    // corr y rms EN el lag refinado (interpolando la referencia en el punto
    // fraccionario: si no, el rms mediria el error de redondeo del lag).
    double num = 0, xx = 0, yy = 0, dif = 0;
    long n = 0;
    const int count = static_cast<int>(out.size());
    for (int i = 0; i < count; ++i)
    {
        const double jr = static_cast<double>(i) - refined;
        const int j0 = static_cast<int>(std::floor(jr));
        if (j0 < 0 || j0 + 1 >= static_cast<int>(ref.size())) continue;
        const double t = jr - static_cast<double>(j0);
        const double b = (1.0 - t) * ref[static_cast<size_t>(j0)] +
                         t * ref[static_cast<size_t>(j0 + 1)];
        const double a = out[static_cast<size_t>(i)];
        num += a * b; xx += a * a; yy += b * b;
        dif += (a - b) * (a - b);
        ++n;
    }

    fit.lag = refined;
    fit.corr = (xx > 0 && yy > 0) ? num / std::sqrt(xx * yy) : 0.0;
    fit.rms = std::sqrt(dif / std::max<long>(1, n));
    fit.n = n;
    fit.ok = n > 0;
    return fit;
}

int countDistinct(const std::vector<float> &a, const std::vector<float> &b)
{
    const size_t n = std::min(a.size(), b.size());
    int distinct = static_cast<int>(a.size() > n ? a.size() - n : 0) +
                   static_cast<int>(b.size() > n ? b.size() - n : 0);
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) ++distinct;
    return distinct;
}

int selfTest()
{
    // Un desplazamiento ENTERO conocido: el estimador tiene que devolverlo.
    const double sr = 48000.0;
    const int n = static_cast<int>(0.5 * sr);
    std::vector<float> ref(static_cast<size_t>(n), 0.f);
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / sr;
        const double env = std::exp(-6.0 * t);
        ref[static_cast<size_t>(i)] =
            static_cast<float>(0.4 * env * (std::sin(2.0 * kPi * 220.0 * t) +
                                            0.3 * std::sin(2.0 * kPi * 613.0 * t)));
    }

    int failures = 0;
    for (int shift : {0, 3, -5})
    {
        std::vector<float> out(static_cast<size_t>(n), 0.f);
        for (int i = 0; i < n; ++i)
        {
            const int j = i - shift;
            if (j >= 0 && j < n) out[static_cast<size_t>(i)] = ref[static_cast<size_t>(j)];
        }
        const LagFit fit = measureLag(out, ref);
        const double expect = static_cast<double>(shift);
        std::printf("selftest lag %+d: medido %+0.3f  corr %.6f  rms %.2e\n",
                    shift, fit.lag, fit.corr, fit.rms);
        if (std::fabs(fit.lag - expect) > 0.05)
        {
            std::printf("FALLA selftest: lag %+0.3f, esperado %+0.1f\n", fit.lag, expect);
            ++failures;
        }
    }
    std::printf("selftest   : %s\n", failures == 0 ? "PASA" : "FALLA");
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// MODO CLI (nivel 3 de D-093, protocolo D-099): la MISMA edicion por los dos
// caminos. La rejilla y la curva las manda un CSV; el arnes construye la
// peticion como la construye render_cli (render_cli.cpp:178-203) y la traduce
// como la traduce el plugin (PitchcoreRequest.h).
// ---------------------------------------------------------------------------

// Copia literal de loadF0 de render_cli (render_cli.cpp:36-56). Si algun dia
// divergen, la comprobacion de curvas byte a byte lo delata antes de medir.
std::vector<pitchcore::F0Frame> loadF0Csv(const std::string &path)
{
    std::vector<pitchcore::F0Frame> out;
    std::ifstream f(path);
    if (!f) return out;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line))
    {
        for (char &ch : line) if (ch == ',') ch = ' ';
        std::istringstream is(line);
        double t = 0, f0 = 0, aper = 0, rms = 0; int v = 0;
        if (!(is >> t >> f0)) continue;
        is >> aper >> rms >> v;
        pitchcore::F0Frame fr;
        fr.timeSec = t; fr.f0Hz = f0; fr.aper = aper; fr.rmsDb = rms;
        fr.voiced = (v != 0) && f0 > 0;
        out.push_back(fr);
    }
    return out;
}

// La curva objetivo con el MISMO formato que escribe render_cli
// (render_cli.cpp:254-268): <salida>.curva.csv. Byte a byte comparable.
bool writeCurveCsv(const std::string &path, const pitchcore::PitchCurve &curve)
{
    std::ofstream f(path);
    if (!f) return false;
    f << "time,f0,source,aper,voiced\n";
    for (size_t i = 0; i < curve.f0Hz.size(); ++i)
        f << curve.timeSec[i] << "," << curve.f0Hz[i] << "," << curve.sourceHz[i]
          << ",1," << (curve.voiced[i] ? 1 : 0) << "\n";
    return true;
}

struct Compare
{
    long n = 0;
    int distinct = 0;
    long firstDistinct = -1;
    LagFit fit;
};

Compare comparePrefix(const std::vector<float> &a, const std::vector<float> &b)
{
    Compare c;
    const size_t n = std::min(a.size(), b.size());
    c.n = static_cast<long>(n);
    std::vector<float> as(a.begin(), a.begin() + n);
    std::vector<float> bs(b.begin(), b.begin() + n);
    c.distinct = countDistinct(as, bs);
    for (size_t i = 0; i < n; ++i)
        if (as[i] != bs[i]) { c.firstDistinct = static_cast<long>(i); break; }
    c.fit = measureLag(as, bs);
    return c;
}

int runCliMode(const std::string &wavPath, const std::string &f0Csv,
               const std::string &curvaCsv, const std::string &outPath,
               const std::string &vsPath, const std::string &curvaOutPath)
{
    pitchcore::WavData wav;
    std::string err;
    if (!pitchcore::readWav(wavPath, wav, &err))
    {
        std::printf("ERROR leyendo %s: %s\n", wavPath.c_str(), err.c_str());
        return 1;
    }
    const double sr = wav.sampleRate;

    const auto frames = loadF0Csv(f0Csv);
    if (frames.empty())
    {
        std::printf("ERROR: el F0 de %s esta vacio\n", f0Csv.c_str());
        return 1;
    }
    const auto cf = loadF0Csv(curvaCsv);
    if (cf.empty())
    {
        std::printf("ERROR: la curva de %s esta vacia\n", curvaCsv.c_str());
        return 1;
    }

    // La curva se construye con el MISMO codigo que el CLI (buildPitchCurve) y
    // se le superpone el CSV objetivo, igual que render_cli.cpp:167-176.
    pitchcore::PitchCurve curve = pitchcore::buildPitchCurve(frames, {}, {});
    int failures = 0;
    if (cf.size() != curve.f0Hz.size())
    {
        std::printf("FALLA curvas: %zu frames en la curva y %zu en el F0; no es la misma rejilla\n",
                    cf.size(), curve.f0Hz.size());
        return 1;
    }
    for (size_t i = 0; i < curve.f0Hz.size(); ++i)
    {
        curve.f0Hz[i] = static_cast<float>(cf[i].f0Hz);
        curve.voiced[i] = cf[i].voiced ? 1 : 0;
    }

    const int nFrames = static_cast<int>(curve.f0Hz.size());
    const double hopSamples = (nFrames > 1) ? (curve.timeSec[1] - curve.timeSec[0]) * sr : 0.0;
    const int hopInt = static_cast<int>(std::lround(hopSamples));

    std::printf("material   : %s\n", wavPath.c_str());
    std::printf("sr         : %.0f Hz    muestras: %zu    frames: %d\n", sr,
                wav.mono.size(), nFrames);
    std::printf("rejilla    : hop %.9f muestras (entero %d)  curva %s\n",
                hopSamples, hopInt, curvaCsv.c_str());
    if (std::fabs(hopSamples - static_cast<double>(hopInt)) > 1e-6 || hopInt <= 0)
    {
        // Sin hop entero, el hopSize del plugin seria otro y no seria la misma
        // edicion (D-099): se para en vez de comparar cosas distintas.
        std::printf("FALLA rejilla: el plugin exige hop entero y este es %.9f\n", hopSamples);
        return 1;
    }

    // Curva objetivo: mismo fichero de valores que ve el CLI.
    if (!curvaOutPath.empty() && !writeCurveCsv(curvaOutPath, curve))
    {
        std::printf("ERROR escribiendo %s\n", curvaOutPath.c_str());
        return 1;
    }

    // --- la edicion, con su control (una edicion vacia no mide nada) ---
    int shifted = 0;
    double maxCents = 0.0;
    double rmsRegion = 0.0;
    for (int i = 0; i < nFrames; ++i)
    {
        const double s = curve.sourceHz[static_cast<size_t>(i)];
        const double t = curve.f0Hz[static_cast<size_t>(i)];
        const double r = (s > 0 && t > 0) ? t / s : 1.0;
        const double cents = (r > 0) ? 1200.0 * std::log(r) / std::log(2.0) : 0.0;
        if (std::fabs(cents) > 0.5) ++shifted;
        if (std::fabs(cents) > std::fabs(maxCents)) maxCents = cents;
    }
    for (float v : wav.mono) rmsRegion += static_cast<double>(v) * v;
    rmsRegion = std::sqrt(rmsRegion / std::max<size_t>(1, wav.mono.size()));
    std::printf("edicion    : %d de %d frames con ratio != 1; max %+.1f cents; rms del material %.3e\n",
                shifted, nFrames, maxCents, rmsRegion);
    if (shifted < 8)
    {
        std::printf("FALLA edicion: no hay edicion que medir (%d frames)\n", shifted);
        ++failures;
    }
    if (rmsRegion < kSilenceRms)
    {
        std::printf("FALLA material: el fichero es silencio (rms %.3e)\n", rmsRegion);
        ++failures;
    }

    // --- peticion del plugin (DspSynthesisRequest) y su traduccion ---
    DspSynthesisRequest dsp;
    dsp.source = wav.mono;             // el tramo cubre el fichero entero
    dsp.sourceStartSample = 0;
    dsp.numOutputFrames = nFrames;
    dsp.hopSize = hopInt;
    dsp.sampleRate = static_cast<int>(sr);
    dsp.sourceFrame.resize(static_cast<size_t>(nFrames));
    dsp.pitchRatio.assign(static_cast<size_t>(nFrames), 1.f);
    dsp.sourceF0.assign(static_cast<size_t>(nFrames), 0.f);
    dsp.sourceVoiced.assign(static_cast<size_t>(nFrames), 0);
    for (int i = 0; i < nFrames; ++i)
    {
        const size_t k = static_cast<size_t>(i);
        dsp.sourceFrame[k] = static_cast<double>(i);   // identidad
        dsp.sourceF0[k] = curve.sourceHz[k];
        dsp.sourceVoiced[k] = curve.sourceVoiced[k] ? 1 : 0;
        const double s = curve.sourceHz[k];
        if (s > 0 && curve.voiced[k] && curve.f0Hz[k] > 0)
            dsp.pitchRatio[k] = static_cast<float>(curve.f0Hz[k] / s);
    }

    const auto core = pitchcore_adapter::toCoreRequest(dsp);
    std::printf("traduccion : curveHop=%.3f totalFrames=%d base=%d salida=%d muestras=%d\n",
                core.curveHop, core.totalFrames, core.totalFrames - dsp.numOutputFrames,
                core.sourceStartSample, core.numSamples);

    // --- peticion del CLI (render_cli.cpp:178-203), dos variantes ---
    // (a) con numSamples = tamano del WAV, que es lo que hace el CLI;
    // (b) con numSamples = frames*hop, el contrato del plugin, para aislar la
    //     traduccion sin la cola de 14/226 muestras.
    auto makeCliRequest = [&](int numSamples)
    {
        pitchcore::RenderRequest req;
        req.original = &wav.mono;
        req.sourceStartSample = 0;
        req.numSamples = numSamples;
        req.frameOffset = 0;
        req.totalFrames = nFrames;
        req.curveHop = hopSamples;
        req.sampleRate = sr;
        req.sourceFrame.resize(static_cast<size_t>(nFrames));
        req.pitchRatio.assign(static_cast<size_t>(nFrames), 1.f);
        req.sourceF0 = dsp.sourceF0;
        req.sourceVoiced = dsp.sourceVoiced;
        for (int i = 0; i < nFrames; ++i)
        {
            req.sourceFrame[static_cast<size_t>(i)] = static_cast<double>(i);
            req.pitchRatio[static_cast<size_t>(i)] = dsp.pitchRatio[static_cast<size_t>(i)];
        }
        return req;
    };

    std::vector<float> outAdapter, outCliShort, outCliFull;
    {
        pitchcore::ClassicEngine engine;
        if (!engine.render(core, outAdapter, err))
        {
            std::printf("ERROR render adaptador: %s\n", err.c_str());
            return 1;
        }
        const auto reqShort = makeCliRequest(core.numSamples);
        if (!engine.render(reqShort, outCliShort, err))
        {
            std::printf("ERROR render CLI-corto: %s\n", err.c_str());
            return 1;
        }
        const auto reqFull = makeCliRequest(static_cast<int>(wav.mono.size()));
        if (!engine.render(reqFull, outCliFull, err))
        {
            std::printf("ERROR render CLI-completo: %s\n", err.c_str());
            return 1;
        }
    }

    std::printf("longitudes : adaptador %zu   CLI(motor, completo) %zu\n",
                outAdapter.size(), outCliFull.size());

    // (iii) aislado: adaptador vs motor con la peticion del CLI y el mismo
    // contrato de longitud. Se espera 0 muestras distintas.
    {
        const Compare c = comparePrefix(outAdapter, outCliShort);
        std::printf("aislado    : adaptador vs motor(CLI) -> %d distintas de %ld (primera %ld)"
                    "  lag %+.2f  corr %.6f  rms %.3e\n",
                    c.distinct, c.n, c.firstDistinct, c.fit.lag, c.fit.corr, c.fit.rms);
        if (c.distinct != 0)
        {
            std::printf("FALLA aislado: la traduccion cambia %d muestras\n", c.distinct);
            ++failures;
        }
    }

    // (ii) control: motor con la peticion del CLI completa vs el WAV del CLI.
    // Mide el mezclador del despachador y la cola; informativo, no criterio.
    if (!vsPath.empty())
    {
        pitchcore::WavData cli;
        if (!pitchcore::readWav(vsPath, cli, &err))
        {
            std::printf("ERROR leyendo %s: %s\n", vsPath.c_str(), err.c_str());
            ++failures;
        }
        else
        {
            const Compare c2 = comparePrefix(outCliFull, cli.mono);
            std::printf("control    : motor(CLI, completo) vs CLI -> %d distintas de %ld"
                        "  lag %+.2f  corr %.6f  rms %.3e\n",
                        c2.distinct, c2.n, c2.fit.lag, c2.fit.corr, c2.fit.rms);

            // (i) EL CRITERIO (D-093 §3): adaptador vs CLI, mismo audio comun.
            const Compare c = comparePrefix(outAdapter, cli.mono);
            const long diff = static_cast<long>(outAdapter.size()) -
                              static_cast<long>(cli.mono.size());
            std::printf("nivel 3    : adaptador vs CLI -> n=%ld  lag %+.2f  corr %.6f  rms %.3e"
                        "  (longitudes %zu vs %zu, delta %+ld)\n",
                        c.n, c.fit.lag, c.fit.corr, c.fit.rms, outAdapter.size(),
                        cli.mono.size(), diff);
            if (c.fit.corr < kCorrMin || c.fit.rms > kRmsMax)
            {
                std::printf("FALLA nivel 3: corr %.6f (min %.4f) o rms %.3e (max %.0e)\n",
                            c.fit.corr, kCorrMin, c.fit.rms, kRmsMax);
                ++failures;
            }
        }
    }

    if (!outPath.empty())
    {
        if (pitchcore::writeWavMono(outPath, outAdapter, sr))
            std::printf("salida     : %s\n", outPath.c_str());
        else
        {
            std::printf("ERROR escribiendo %s\n", outPath.c_str());
            ++failures;
        }
    }

    std::printf("veredicto  : %s\n",
                failures == 0 ? "PASA (los criterios pedidos)" : "FALLA");
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// MODO FIXTURA (nivel 4, D-103): la peticion REAL que recibe el motor, volcada
// por el adaptador, ejecutada aqui por la MISMA traduccion (toCoreRequest) y el
// MISMO motor (ClassicEngine). Cuenta las marcas de periodo del tramo de la
// nota y mide si la salida desplaza. El criterio esta escrito ANTES de contar
// (D-103); aqui solo se aplica.
// ---------------------------------------------------------------------------

// Residuo del adaptador con esta peticion (D-102, 2c: vsOriginal rms=9,466e-05).
constexpr double kAdapterRms = 9.466e-05;
constexpr double kFidelityTol = 1e-6;   // control de fidelidad de la fixtura
constexpr double kNoShiftRms = 1e-3;    // 10x el residuo del adaptador
constexpr double kShiftRms = 1e-2;      // ~8x por debajo de una edicion real (8,6e-02)

bool readFixture(const std::string &path, DspSynthesisRequest &req, std::string &err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        err = "no se puede abrir";
        return false;
    }
    char magic[8] = {};
    f.read(magic, 8);
    if (!f || std::memcmp(magic, "PCREQ1", 6) != 0)
    {
        err = "no es una fixtura PCREQ1";
        return false;
    }
    auto r32 = [&f]() -> int32_t {
        int32_t v = 0;
        f.read(reinterpret_cast<char *>(&v), 4);
        return v;
    };
    const int32_t version = r32();
    if (version != 1)
    {
        err = "version distinta de 1";
        return false;
    }
    const int32_t hop = r32();
    const int32_t sr = r32();
    const int32_t src0 = r32();
    const int32_t frames = r32();
    uint8_t formants = 0;
    f.read(reinterpret_cast<char *>(&formants), 1);
    const int32_t nSource = r32();
    const int32_t nFrame = r32();
    const int32_t nRatio = r32();
    const int32_t nF0 = r32();
    const int32_t nVoiced = r32();
    if (!f)
    {
        err = "cabecera truncada";
        return false;
    }
    if (hop <= 0 || sr <= 0 || frames <= 0 || nSource <= 0 ||
        nFrame < 0 || nRatio < 0 || nF0 < 0 || nVoiced < 0)
    {
        err = "cabecera invalida";
        return false;
    }

    req.hopSize = hop;
    req.sampleRate = sr;
    req.sourceStartSample = src0;
    req.numOutputFrames = frames;
    req.preserveFormants = formants != 0;
    req.source.resize(static_cast<size_t>(nSource));
    req.sourceFrame.resize(static_cast<size_t>(nFrame));
    req.pitchRatio.resize(static_cast<size_t>(nRatio));
    req.sourceF0.resize(static_cast<size_t>(nF0));
    req.sourceVoiced.resize(static_cast<size_t>(nVoiced));

    auto rd = [&f](void *p, size_t bytes) {
        if (bytes > 0)
            f.read(static_cast<char *>(p), static_cast<std::streamsize>(bytes));
    };
    rd(req.source.data(), req.source.size() * sizeof(float));
    rd(req.sourceFrame.data(), req.sourceFrame.size() * sizeof(double));
    rd(req.pitchRatio.data(), req.pitchRatio.size() * sizeof(float));
    rd(req.sourceF0.data(), req.sourceF0.size() * sizeof(float));
    rd(req.sourceVoiced.data(), req.sourceVoiced.size());
    if (!f)
    {
        err = "cuerpo truncado";
        return false;
    }
    return true;
}

int runFixtureMode(const std::string &path, const std::string &outPath,
                   int notaIni, int notaFin)
{
    DspSynthesisRequest dsp;
    std::string err;
    if (!readFixture(path, dsp, err))
    {
        std::printf("ERROR fixtura %s: %s\n", path.c_str(), err.c_str());
        return 1;
    }

    float rmin = 1.0f, rmax = 1.0f;
    double rsum = 0.0;
    for (float r : dsp.pitchRatio)
    {
        rmin = std::min(rmin, r);
        rmax = std::max(rmax, r);
        rsum += static_cast<double>(r);
    }
    std::printf("fixtura    : %s\n", path.c_str());
    std::printf("peticion   : frames=%d hop=%d sr=%d src0=%d src=%zu\n",
                dsp.numOutputFrames, dsp.hopSize, dsp.sampleRate,
                dsp.sourceStartSample, dsp.source.size());
    std::printf("curvas     : sourceFrame=%zu ratio=%zu (media %.4f, rango [%.4f,%.4f])"
                " f0=%zu voiced=%zu\n",
                dsp.sourceFrame.size(), dsp.pitchRatio.size(),
                dsp.pitchRatio.empty() ? 1.0
                                       : rsum / static_cast<double>(dsp.pitchRatio.size()),
                rmin, rmax, dsp.sourceF0.size(), dsp.sourceVoiced.size());

    const pitchcore::RenderRequest core = pitchcore_adapter::toCoreRequest(dsp);
    const int base = core.totalFrames - dsp.numOutputFrames;
    std::printf("traduccion : curveHop=%.0f totalFrames=%d base=%d origen=%d muestras=%d\n",
                core.curveHop, core.totalFrames, base, core.sourceStartSample,
                core.numSamples);

    const int fMid = (notaIni + notaFin) / 2;
    const int localMid = fMid - base;
    if (localMid >= 0 && localMid < static_cast<int>(dsp.pitchRatio.size()))
    {
        const float r = dsp.pitchRatio[static_cast<size_t>(localMid)];
        std::printf("ratio nota : frame %d -> %.4f (%+.1f c)\n", fMid, r,
                    1200.0 * std::log2(static_cast<double>(r)));
    }

    const std::vector<int> marcas = pitchcore::ClassicEngine::detectPitchMarks(core);
    std::printf("marcas     : %zu en toda la peticion\n", marcas.size());

    const long notaA = static_cast<long>(notaIni) * dsp.hopSize;
    const long notaB = static_cast<long>(notaFin) * dsp.hopSize;
    int enNota = 0;
    long primera = -1, ultima = -1;
    for (int m : marcas)
    {
        if (m >= notaA && m < notaB)
        {
            if (primera < 0)
                primera = m;
            ultima = m;
            ++enNota;
        }
    }
    std::printf("nota       : frames [%d,%d) = muestras [%ld,%ld) -> %d marcas",
                notaIni, notaFin, notaA, notaB, enNota);
    if (enNota >= 2 && ultima > primera)
        std::printf(" (paso medio %.1f muestras)",
                    static_cast<double>(ultima - primera) /
                        static_cast<double>(enNota - 1));
    std::printf("\n");

    std::vector<float> out;
    if (!pitchcore::ClassicEngine().render(core, out, err))
    {
        std::printf("ERROR motor: %s\n", err.c_str());
        return 1;
    }
    std::printf("salida     : %zu muestras (contrato %d)  err='%s'\n",
                out.size(), dsp.numOutputFrames * dsp.hopSize, err.c_str());

    const long outOrigin = static_cast<long>(core.sourceStartSample);
    const long srcOrigin = static_cast<long>(dsp.sourceStartSample);
    auto rmsWindow = [&](long aAbs, long bAbs, double *maxAbs, long *count) {
        double e = 0.0, mx = 0.0;
        long n = 0;
        for (long abs = aAbs; abs < bAbs; ++abs)
        {
            const long i = abs - outOrigin;
            const long k = abs - srcOrigin;
            if (i < 0 || i >= static_cast<long>(out.size()))
                continue;
            if (k < 0 || k >= static_cast<long>(dsp.source.size()))
                continue;
            const double d = static_cast<double>(out[static_cast<size_t>(i)]) -
                             static_cast<double>(dsp.source[static_cast<size_t>(k)]);
            e += d * d;
            mx = std::max(mx, std::fabs(d));
            ++n;
        }
        if (maxAbs != nullptr)
            *maxAbs = mx;
        if (count != nullptr)
            *count = n;
        return n > 0 ? std::sqrt(e / static_cast<double>(n)) : -1.0;
    };

    double maxAll = 0.0, maxNota = 0.0;
    long nAll = 0, nNota = 0;
    const double rmsAll = rmsWindow(outOrigin, outOrigin + static_cast<long>(out.size()),
                                    &maxAll, &nAll);
    const double rmsNota = rmsWindow(notaA, notaB, &maxNota, &nNota);
    std::printf("vs original: rms %.6e max %.6e n %ld  (toda la peticion)\n",
                rmsAll, maxAll, nAll);
    std::printf("             rms %.6e max %.6e n %ld  (nota)\n",
                rmsNota, maxNota, nNota);

    const bool sizeOk = static_cast<int>(out.size()) == dsp.numOutputFrames * dsp.hopSize;
    const bool rmsOk = rmsAll >= 0.0 && std::fabs(rmsAll - kAdapterRms) <= kFidelityTol;
    std::printf("fidelidad  : tamano %s ; rms contra el adaptador (%.6e) %s\n",
                sizeOk ? "PASA" : "FALLA", kAdapterRms, rmsOk ? "PASA" : "FALLA");

    if (!outPath.empty() && pitchcore::writeWavMono(outPath, out, dsp.sampleRate))
        std::printf("salida wav : %s\n", outPath.c_str());

    if (!sizeOk || !rmsOk)
    {
        std::printf("nivel 4    : SIN VEREDICTO (la fixtura no reproduce la peticion real)\n");
        return 1;
    }

    const char *fila = nullptr;
    if (enNota < 2)
        fila = "marcas del tramo < 2 -> el fallo esta en lo que RECIBE el motor";
    else if (rmsNota >= 0.0 && rmsNota < kNoShiftRms)
        fila = "marcas >= 2 y NO desplaza -> render PSOLA del nucleo";
    else if (rmsNota >= kShiftRms)
        fila = "marcas >= 2 y desplaza -> la entrega en el plugin (peticion vs llamada)";
    else
        fila = "zona sin veredicto (rms en la nota entre 1e-3 y 1e-2)";
    std::printf("nivel 4    : %s\n", fila);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::printf("uso: %s <wav> [cents] [startSec] [durSec] "
                    "[--out salida.wav] [--vs ref.wav] [--identical otra.wav] [--selftest]\n"
                    "   o: %s <wav> --f0 f0.csv --curve curva.csv [--curva-out c.csv] "
                    "[--out salida.wav] [--vs cli.wav]   (nivel 3, D-099)\n"
                    "   o: %s --compare <a.wav> <b.wav>   (bounces 2a/2c, D-100)\n"
                    "   o: %s --fixture peticion.bin [--nota-frames ini fin] [--out salida.wav]"
                    "   (nivel 4, D-103)\n",
                    argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    if (std::string(argv[1]) == "--selftest")
        return selfTest();

    // Comparacion de dos WAV ya renderizados (bounces 2a/2c, D-100): no
    // renderiza nada, mide muestra a muestra y con el mismo estimador de lag.
    if (std::string(argv[1]) == "--compare")
    {
        if (argc < 4)
        {
            std::printf("uso: %s --compare <a.wav> <b.wav>\n", argv[0]);
            return 2;
        }
        pitchcore::WavData a, b;
        std::string err;
        if (!pitchcore::readWav(argv[2], a, &err))
        {
            std::printf("ERROR leyendo %s: %s\n", argv[2], err.c_str());
            return 1;
        }
        if (!pitchcore::readWav(argv[3], b, &err))
        {
            std::printf("ERROR leyendo %s: %s\n", argv[3], err.c_str());
            return 1;
        }
        const int common = static_cast<int>(std::min(a.mono.size(), b.mono.size()));
        const int distinct = countDistinct(a.mono, b.mono);
        const LagFit fit = measureLag(a.mono, b.mono);
        std::printf("A: %s  %d muestras  %.0f Hz\n", argv[2],
                    static_cast<int>(a.mono.size()), a.sampleRate);
        std::printf("B: %s  %d muestras  %.0f Hz\n", argv[3],
                    static_cast<int>(b.mono.size()), b.sampleRate);
        std::printf("muestras distintas: %d de %d\n", distinct, common);
        std::printf("lag(A vs B): %.2f  corr %.6f  rms %.3e  n %ld\n",
                    fit.lag, fit.corr, fit.rms, fit.n);
        return 0;
    }

    // Nivel 4 (D-103): la peticion REAL volcada por el adaptador, ejecutada por
    // la misma traduccion y el mismo motor que el plugin.
    if (std::string(argv[1]) == "--fixture")
    {
        if (argc < 3)
        {
            std::printf("uso: %s --fixture peticion.bin [--nota-frames ini fin] [--out salida.wav]\n",
                        argv[0]);
            return 2;
        }
        std::string outPath;
        int notaIni = 1838;
        int notaFin = 1892;
        for (int i = 3; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--out" && i + 1 < argc)
            {
                outPath = argv[++i];
                continue;
            }
            if (a == "--nota-frames" && i + 2 < argc)
            {
                notaIni = std::atoi(argv[++i]);
                notaFin = std::atoi(argv[++i]);
                continue;
            }
            std::printf("ERROR: argumento desconocido %s\n", a.c_str());
            return 2;
        }
        return runFixtureMode(argv[2], outPath, notaIni, notaFin);
    }

    const std::string path = argv[1];
    double cents = 100.0;
    double startSec = 0.0;
    double durSec = 2.0;
    std::string outPath;
    std::string vsPath;
    std::string identicalPath;
    std::string f0Csv;
    std::string curvaCsv;
    std::string curvaOutPath;
    int positional = 0;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) { outPath = argv[++i]; continue; }
        if (a == "--vs" && i + 1 < argc) { vsPath = argv[++i]; continue; }
        if (a == "--identical" && i + 1 < argc) { identicalPath = argv[++i]; continue; }
        if (a == "--f0" && i + 1 < argc) { f0Csv = argv[++i]; continue; }
        if (a == "--curve" && i + 1 < argc) { curvaCsv = argv[++i]; continue; }
        if (a == "--curva-out" && i + 1 < argc) { curvaOutPath = argv[++i]; continue; }
        if (a == "--selftest") return selfTest();
        ++positional;
        const double v = std::atof(a.c_str());
        if (positional == 1) cents = v;
        else if (positional == 2) startSec = v;
        else if (positional == 3) durSec = v;
    }

    // Modo CLI (nivel 3, D-099): hacen falta el F0 y la curva objetivo.
    if (!f0Csv.empty() || !curvaCsv.empty())
    {
        if (f0Csv.empty() || curvaCsv.empty())
        {
            std::printf("ERROR: --f0 y --curve van juntos\n");
            return 2;
        }
        return runCliMode(path, f0Csv, curvaCsv, outPath, vsPath, curvaOutPath);
    }

    pitchcore::WavData wav;
    std::string err;
    if (!pitchcore::readWav(path, wav, &err))
    {
        std::printf("ERROR leyendo %s: %s\n", path.c_str(), err.c_str());
        return 1;
    }

    const double sr = wav.sampleRate;
    const int totalSamples = static_cast<int>(wav.mono.size());
    const int totalFrames = totalSamples / kHop;

    pitchcore::F0Params fp;
    fp.sampleRate = sr;
    const auto f0Frames = pitchcore::estimateF0(wav.mono, fp);
    const Grid grid = makeGrid(f0Frames, totalFrames, sr);

    const int startFrame = std::max(0, static_cast<int>(std::floor(startSec * sr / kHop)));
    const int endFrame = std::min(totalFrames,
                                  static_cast<int>(std::ceil((startSec + durSec) * sr / kHop)));
    if (endFrame - startFrame < 8)
    {
        std::printf("ERROR: region demasiado corta (%d frames)\n", endFrame - startFrame);
        return 1;
    }

    DspSynthesisRequest req;
    req.hopSize = kHop;
    req.sampleRate = sr;
    req.numOutputFrames = endFrame - startFrame;
    req.sourceFrame.resize(static_cast<size_t>(req.numOutputFrames));
    req.pitchRatio.assign(static_cast<size_t>(req.numOutputFrames), 1.f);

    const double ratio = std::pow(2.0, cents / 1200.0);
    double minSource = 1e18, maxSource = -1e18;
    for (int i = 0; i < req.numOutputFrames; ++i)
    {
        const double source = static_cast<double>(startFrame + i);
        req.sourceFrame[static_cast<size_t>(i)] = source;
        minSource = std::min(minSource, source);
        maxSource = std::max(maxSource, source);
        const size_t k = static_cast<size_t>(std::min(totalFrames - 1,
                                                      static_cast<int>(std::llround(source))));
        req.pitchRatio[static_cast<size_t>(i)] =
            (grid.voiced[k] && grid.f0[k] > 0.f) ? static_cast<float>(ratio) : 1.f;
    }

    const int sourceStart = std::clamp(
        static_cast<int>(std::floor(minSource)) * kHop - kContext, 0, totalSamples);
    const int sourceEnd = std::clamp(
        (static_cast<int>(std::ceil(maxSource)) + 1) * kHop + kContext, 0, totalSamples);
    if (sourceEnd <= sourceStart)
    {
        std::printf("ERROR: tramo vacio\n");
        return 1;
    }
    req.source.assign(wav.mono.begin() + sourceStart, wav.mono.begin() + sourceEnd);
    req.sourceStartSample = sourceStart;
    req.sourceF0 = grid.f0;
    req.sourceVoiced = grid.voiced;

    std::printf("material   : %s\n", path.c_str());
    std::printf("sr         : %.0f Hz    muestras: %d    frames: %d\n", sr,
                totalSamples, totalFrames);
    std::printf("region     : frames [%d,%d)  (%d frames, %.3f s)\n", startFrame,
                endFrame, req.numOutputFrames,
                static_cast<double>(req.numOutputFrames) * kHop / sr);
    std::printf("tramo      : muestras [%d,%d)  (%d)  originalStartSample=%d\n",
                sourceStart, sourceEnd, sourceEnd - sourceStart, sourceStart);
    std::printf("shift      : %+.1f cents\n", cents);

    const auto core = pitchcore_adapter::toCoreRequest(req);
    std::printf("traduccion : curveHop=%.0f totalFrames=%d base=%d  salida=%d\n",
                core.curveHop, core.totalFrames,
                core.totalFrames - req.numOutputFrames, core.sourceStartSample);

    pitchcore::ClassicEngine engine;
    std::vector<float> out;
    std::string renderErr;
    if (!engine.render(core, out, renderErr))
    {
        std::printf("ERROR render: %s\n", renderErr.c_str());
        return 1;
    }

    const int expected = req.numOutputFrames * req.hopSize;
    int failures = 0;

    // CONTROL DE MATERIAL: una region de silencio "pasa" cualquier comparacion.
    {
        const long a = startFrame * kHop;
        double energy = 0;
        long n = 0;
        for (int i = 0; i < expected; ++i)
        {
            if (a + i < 0 || a + i >= totalSamples) continue;
            const double x = wav.mono[static_cast<size_t>(a + i)];
            energy += x * x;
            ++n;
        }
        const double regionRms = std::sqrt(energy / std::max<long>(1, n));
        std::printf("nivel      : rms original en la region %.3e\n", regionRms);
        if (regionRms < kSilenceRms)
        {
            std::printf("FALLA material: la region es silencio (rms %.3e < %.0e)\n",
                        regionRms, kSilenceRms);
            ++failures;
        }
    }

    if (static_cast<int>(out.size()) != expected)
    {
        std::printf("FALLA longitud: %zu muestras, contrato pide %d\n", out.size(), expected);
        ++failures;
    }

    // NIVEL 1: con 0 cents la salida es el original, y el lag tiene que ser 0.
    if (std::fabs(cents) < 1e-9 && static_cast<int>(out.size()) == expected)
    {
        const long a = startFrame * kHop;
        std::vector<float> ref(out.size(), 0.f);
        bool hasRef = false;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const long j = a + static_cast<long>(i);
            if (j < 0 || j >= totalSamples) continue;
            ref[i] = wav.mono[static_cast<size_t>(j)];
            hasRef = true;
        }
        if (hasRef)
        {
            const LagFit fit = measureLag(out, ref);
            std::printf("nivel 1    : lag %+.2f  corr %.6f  rms %.3e  (n=%ld)\n",
                        fit.lag, fit.corr, fit.rms, fit.n);
            if (fit.corr < kCorrMin || std::fabs(fit.lag) > kLagTol)
            {
                std::printf("FALLA nivel 1: corr %.6f (min %.4f) o lag %+.2f (tol %.2f)\n",
                            fit.corr, kCorrMin, fit.lag, kLagTol);
                ++failures;
            }
        }
        else
        {
            std::printf("FALLA nivel 1: no hay referencias dentro del archivo\n");
            ++failures;
        }
    }

    // NIVEL 3: contra una referencia (el render del CLI) por alineacion
    // fraccional. Sin C1: el bounce no se mide por C1 (D-094).
    if (!vsPath.empty())
    {
        pitchcore::WavData refWav;
        std::string refErr;
        if (!pitchcore::readWav(vsPath, refWav, &refErr))
        {
            std::printf("ERROR leyendo %s: %s\n", vsPath.c_str(), refErr.c_str());
            ++failures;
        }
        else if (refWav.mono.size() != out.size())
        {
            std::printf("FALLA nivel 3: longitudes %zu y %zu; no son la misma edicion\n",
                        out.size(), refWav.mono.size());
            ++failures;
        }
        else
        {
            const LagFit fit = measureLag(out, refWav.mono);
            std::printf("nivel 3    : vs %s -> lag %+.2f  corr %.6f  rms %.3e\n",
                        vsPath.c_str(), fit.lag, fit.corr, fit.rms);
            if (fit.corr < kCorrMin || fit.rms > kRmsMax)
            {
                std::printf("FALLA nivel 3: corr %.6f (min %.4f) o rms %.3e (max %.0e)\n",
                            fit.corr, kCorrMin, fit.rms, kRmsMax);
                ++failures;
            }
        }
    }

    // NIVEL 2: bit a bit contra otro WAV (PSOLA con el adaptador compilado y
    // sin seleccionar el motor).
    if (!identicalPath.empty())
    {
        pitchcore::WavData other;
        std::string otherErr;
        if (!pitchcore::readWav(identicalPath, other, &otherErr))
        {
            std::printf("ERROR leyendo %s: %s\n", identicalPath.c_str(), otherErr.c_str());
            ++failures;
        }
        else
        {
            const int distinct = countDistinct(out, other.mono);
            std::printf("nivel 2    : vs %s -> %d muestras distintas (de %zu)\n",
                        identicalPath.c_str(), distinct, out.size());
            if (distinct != 0)
            {
                std::printf("FALLA nivel 2: %d muestras distintas, el criterio pide 0\n",
                            distinct);
                ++failures;
            }
        }
    }

    if (!outPath.empty())
    {
        if (pitchcore::writeWavMono(outPath, out, sr))
            std::printf("salida     : %s\n", outPath.c_str());
        else
        {
            std::printf("ERROR escribiendo %s\n", outPath.c_str());
            ++failures;
        }
    }

    std::printf("veredicto  : %s\n",
                failures == 0 ? "PASA (los criterios pedidos)" : "FALLA");
    return failures == 0 ? 0 : 1;
}

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
#include "pitchcore/psola.h"
#include "pitchcore/wav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::printf("uso: %s <wav> [cents] [startSec] [durSec] "
                    "[--out salida.wav] [--vs ref.wav] [--identical otra.wav] [--selftest]\n",
                    argv[0]);
        return 2;
    }
    if (std::string(argv[1]) == "--selftest")
        return selfTest();

    const std::string path = argv[1];
    double cents = 100.0;
    double startSec = 0.0;
    double durSec = 2.0;
    std::string outPath;
    std::string vsPath;
    std::string identicalPath;
    int positional = 0;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) { outPath = argv[++i]; continue; }
        if (a == "--vs" && i + 1 < argc) { vsPath = argv[++i]; continue; }
        if (a == "--identical" && i + 1 < argc) { identicalPath = argv[++i]; continue; }
        if (a == "--selftest") return selfTest();
        ++positional;
        const double v = std::atof(a.c_str());
        if (positional == 1) cents = v;
        else if (positional == 2) startSec = v;
        else if (positional == 3) durSec = v;
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

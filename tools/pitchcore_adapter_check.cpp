// Arnes del adaptador de M2 (D-093, nivel 1).
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
//   1. comprueba el contrato de longitud: out.size() == numOutputFrames*hop;
//   2. con 0 cents comprueba que la traduccion NO desplaza el audio: la salida
//      tiene que ser el original (corr ~1, rms diff ~0). Un error de base en el
//      indice de las curvas o en originalStartSample sale aqui como un
//      desplazamiento de N muestras, no como "suena distinto".
//   3. escribe la salida en .wav (--out) para que el porton de Python mida C1.
//
// Uso:
//   pitchcore_adapter_check <wav> [cents] [startSec] [durSec] [--out salida.wav]

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

constexpr int kHop = 512;
constexpr int kContext = 4096;

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
        // la rejilla de F0 es uniforme y ordenada: avanza sin buscar desde 0
        while (nearest + 1 < frames.size() &&
               std::fabs(frames[nearest + 1].timeSec - t) <
                   std::fabs(frames[nearest].timeSec - t))
            ++nearest;
        g.f0[static_cast<size_t>(k)] = static_cast<float>(frames[nearest].f0Hz);
        g.voiced[static_cast<size_t>(k)] = frames[nearest].voiced ? 1 : 0;
    }
    return g;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::printf("uso: %s <wav> [cents] [startSec] [durSec] [--out salida.wav]\n",
                    argv[0]);
        return 2;
    }

    const std::string path = argv[1];
    double cents = 100.0;
    double startSec = 0.0;
    double durSec = 2.0;
    std::string outPath;
    int positional = 0;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)
        {
            outPath = argv[++i];
            continue;
        }
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

    // Identidad: cada frame de salida lee el mismo frame de la fuente, y el
    // ratio sale de la curva objetivo (+cents sobre el F0 de la fuente), igual
    // que hace el plugin por nota.
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

    // Tramo como lo prepara el fork: contexto a cada lado, clampado al archivo.
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
    std::printf("traduccion : curveHop=%.0f totalFrames=%d base=%d\n", core.curveHop,
                core.totalFrames, core.totalFrames - req.numOutputFrames);

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

    // Control de material: una region de silencio "pasa" cualquier comparacion.
    // Se mide el RMS del original en la region y se exige senal.
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
        if (regionRms < 1e-4)
        {
            std::printf("FALLA material: la region es silencio (rms %.3e), no mide nada\n",
                        regionRms);
            ++failures;
        }
    }

    if (static_cast<int>(out.size()) != expected)
    {
        std::printf("FALLA longitud: %zu muestras, contrato pide %d\n", out.size(), expected);
        ++failures;
    }

    // Comprobacion de alineacion: con 0 cents y fuente identidad la salida ES
    // el original en ese tramo. Un desplazamiento de base aparece aqui.
    if (std::fabs(cents) < 1e-9 && static_cast<int>(out.size()) == expected)
    {
        const long a = startFrame * kHop; // posicion absoluta del primer sample
        double num = 0, xx = 0, yy = 0, dif = 0;
        long n = 0;
        for (int i = 0; i < expected; ++i)
        {
            if (a + i < 0 || a + i >= totalSamples) continue;
            const double x = wav.mono[static_cast<size_t>(a + i)];
            const double y = out[static_cast<size_t>(i)];
            num += x * y; xx += x * x; yy += y * y;
            dif += (x - y) * (x - y);
            ++n;
        }
        const double corr = (xx > 0 && yy > 0) ? num / std::sqrt(xx * yy) : 1.0;
        const double rms = std::sqrt(dif / std::max<long>(1, n));
        std::printf("alineacion : corr %.6f   rms diff %.3e   (n=%ld)\n", corr, rms, n);
        if (corr < 0.9999)
        {
            std::printf("FALLA alineacion: corr %.6f < 0.9999\n", corr);
            ++failures;
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

    std::printf("veredicto  : %s\n", failures == 0 ? "PASA (contrato y alineacion)" : "FALLA");
    return failures == 0 ? 0 : 1;
}

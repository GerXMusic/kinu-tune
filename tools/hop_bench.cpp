// P2 — banco de coste por hop (criterio v2, plan/m2/p2-hop-256-vs-512-criterio.md).
// Mide SOLO el render del motor, con el mismo audio en las dos filas:
// 593 frames x 512 = 1186 frames x 256 = 303.616 muestras.
// Fuente: A/1 real (Renders\Clase A\1-Original.wav), ventana de 303.616 muestras de
// mayor energia. Desplazamiento constante -105,5 c (la edicion de D-092).
// No toca el plugin, no carga vocoder y no cambia HOP_SIZE: el hop va en request.hopSize.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "../Source/Audio/Synthesis/PitchcoreRequest.h"
#include "../Source/Audio/Synthesis/PsolaSynthesizer.h"
#include "pitchcore/f0.h"
#include "pitchcore/psola.h"
#include "pitchcore/wav.h"

namespace
{
constexpr int kSamples = 303616; // 593*512 == 1186*256
constexpr int kReps = 7;
constexpr int kWarmup = 2;

// F0 por autocorrelacion, suficiente para que PSOLA coloque granos de verdad.
float frameF0 (const std::vector<float> &x, int center, double sr)
{
    const int win = 1024;
    const int lagMin = (int) (sr / 400.0), lagMax = (int) (sr / 60.0);
    const int a = std::max (0, center - win / 2);
    const int b = std::min ((int) x.size() - lagMax, a + win);
    if (b - a < win)
        return 0.0f;
    double e = 0.0;
    for (int i = a; i < b; ++i)
        e += (double) x[(size_t) i] * x[(size_t) i];
    if (e / win < 1e-6)
        return 0.0f;

    double best = 0.0;
    int bestLag = 0;
    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        double num = 0.0, d1 = 0.0, d2 = 0.0;
        for (int i = a; i < b; ++i)
        {
            const double p = x[(size_t) i], q = x[(size_t) (i + lag)];
            num += p * q;
            d1 += p * p;
            d2 += q * q;
        }
        const double d = std::sqrt (d1 * d2);
        const double r = d > 0.0 ? num / d : 0.0;
        if (r > best)
        {
            best = r;
            bestLag = lag;
        }
    }
    if (best < 0.4 || bestLag == 0)
        return 0.0f;
    return (float) (sr / bestLag);
}

std::vector<float> pickWindow (const std::vector<float> &mono)
{
    if ((int) mono.size() <= kSamples)
        return std::vector<float> (mono.begin(), mono.end());
    int bestStart = 0;
    double bestE = -1.0;
    const int step = 22050;
    for (int s = 0; s + kSamples <= (int) mono.size(); s += step)
    {
        double e = 0.0;
        for (int i = s; i < s + kSamples; i += 8)
            e += (double) mono[(size_t) i] * mono[(size_t) i];
        if (e > bestE)
        {
            bestE = e;
            bestStart = s;
        }
    }
    return std::vector<float> (mono.begin() + bestStart, mono.begin() + bestStart + kSamples);
}

DspSynthesisRequest makeRequest (const std::vector<float> &src, double sr, int hop,
                                 double cents)
{
    const int frames = (int) src.size() / hop;
    const float ratio = (float) std::pow (2.0, cents / 1200.0);
    DspSynthesisRequest r;
    r.sampleRate = (int) sr;
    r.hopSize = hop;
    r.numOutputFrames = frames;
    r.sourceStartSample = 0;
    r.source = src;
    r.sourceFrame.resize ((size_t) frames);
    r.pitchRatio.assign ((size_t) frames, ratio);
    r.sourceF0.resize ((size_t) frames);
    r.sourceVoiced.resize ((size_t) frames);
    for (int j = 0; j < frames; ++j)
    {
        r.sourceFrame[(size_t) j] = (double) j;
        const float f = frameF0 (src, j * hop + hop / 2, sr);
        r.sourceF0[(size_t) j] = f;
        r.sourceVoiced[(size_t) j] = f > 0.0f ? 1 : 0;
    }
    return r;
}

struct Run
{
    bool ok = false;
    size_t size = 0;
    std::string err;
};

Run runCore (const DspSynthesisRequest &req)
{
    const pitchcore::RenderRequest core = pitchcore_adapter::toCoreRequest (req);
    std::vector<float> out;
    std::string err;
    pitchcore::ClassicEngine engine;
    const bool ok = engine.render (core, out, err);
    return { ok, out.size(), ok ? std::string() : err };
}

Run runPsola (const DspSynthesisRequest &req)
{
    const std::vector<float> out = PsolaSynthesizer::render (req);
    return { ! out.empty(), out.size(), out.empty() ? "empty" : "" };
}

double median (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
} // namespace

int main (int argc, char **argv)
{
    if (argc < 3)
    {
        std::printf ("uso: hop_bench <out.csv|-> <wav> [cents=-105.5]\n");
        return 2;
    }
    const double cents = argc > 3 ? std::atof (argv[3]) : -105.5;

    pitchcore::WavData wav;
    std::string werr;
    if (! pitchcore::readWav (argv[2], wav, &werr))
    {
        std::printf ("FAIL no puedo leer %s: %s\n", argv[2], werr.c_str());
        return 3;
    }
    if ((int) wav.mono.size() < kSamples)
    {
        std::printf ("FAIL %s tiene %zu muestras; hacen falta %d\n", argv[2],
                     wav.mono.size(), kSamples);
        return 3;
    }
    const std::vector<float> src = pickWindow (wav.mono);
    const double sr = wav.sampleRate;

    const bool toFile = std::string (argv[1]) != "-";
    FILE *f = toFile ? std::fopen (argv[1], "w") : stdout;
    if (! f)
    {
        std::printf ("FAIL no puedo escribir %s\n", argv[1]);
        return 2;
    }
    auto out = [&](const char *fmt, auto... a) { std::fprintf (f, fmt, a...); };

    out ("engine,hop,frames,samples,phase,rep,wall_ms,cpu_ms,cpu_ratio,out_size\n");
    out ("# wav=%s sr=%.0f muestras=%d cents=%.1f ratio=%.6f\n", argv[2], sr, kSamples,
         cents, std::pow (2.0, cents / 1200.0));

    const int hops[2] = { 512, 256 };
    std::vector<double> wall[2][2], cpur[2][2];
    for (int e = 0; e < 2; ++e)
    {
        const char *engine = e == 0 ? "PSOLA" : "PITCHCORE";
        for (int h = 0; h < 2; ++h)
        {
            const int hop = hops[h];
            const int frames = kSamples / hop;
            const DspSynthesisRequest req = makeRequest (src, sr, hop, cents);
            Run last;
            for (int i = 0; i < kWarmup + kReps; ++i)
            {
                const bool warm = i < kWarmup;
                const std::clock_t c0 = std::clock();
                const auto t0 = std::chrono::steady_clock::now();
                last = e == 0 ? runPsola (req) : runCore (req);
                const auto t1 = std::chrono::steady_clock::now();
                const std::clock_t c1 = std::clock();
                const double w = std::chrono::duration<double, std::milli> (t1 - t0).count();
                const double c = 1000.0 * (double) (c1 - c0) / (double) CLOCKS_PER_SEC;
                const double q = w > 0.0 ? c / w : 0.0;
                out ("%s,%d,%d,%d,%s,%d,%.3f,%.3f,%.4f,%zu\n", engine, hop, frames,
                     frames * hop, warm ? "warmup" : "rep", i, w, c, q, last.size);
                if (! warm)
                {
                    wall[e][h].push_back (w);
                    cpur[e][h].push_back (q);
                }
            }
            if (! last.ok || (int) last.size != frames * hop)
                out ("FAIL,%s,%d,size=%zu,expected=%d,err=%s\n", engine, hop, last.size,
                     frames * hop, last.err.c_str());
        }
    }

    out ("\n# resumen (2 warm-up + %d reps, mismo audio %d muestras). cpu_ratio>1.2 = multihilo\n",
         kReps, kSamples);
    out ("engine,hop,median_ms,min_ms,max_ms,median_cpu_ratio,hilos\n");
    for (int e = 0; e < 2; ++e)
        for (int h = 0; h < 2; ++h)
        {
            auto v = wall[e][h];
            std::sort (v.begin(), v.end());
            const double q = median (cpur[e][h]);
            out ("%s,%d,%.3f,%.3f,%.3f,%.4f,%s\n", e == 0 ? "PSOLA" : "PITCHCORE", hops[h],
                 median (wall[e][h]), v.front(), v.back(), q, q > 1.2 ? "SI" : "NO");
        }
    for (int e = 0; e < 2; ++e)
    {
        const double a = median (wall[e][0]), b = median (wall[e][1]);
        out ("RATIO,%s,t256/t512=%.4f,techo44.5=%s,ratio1.30=%s\n",
             e == 0 ? "PSOLA" : "PITCHCORE", a > 0.0 ? b / a : 0.0,
             (b <= 44.5 ? "PASA" : "FALLA"), (a > 0.0 && b / a <= 1.30 ? "PASA" : "FALLA"));
    }
    if (toFile) std::fclose (f);
    return 0;
}

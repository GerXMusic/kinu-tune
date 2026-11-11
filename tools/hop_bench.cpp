// P2 — banco de coste por hop (criterio v2, plan/m2/p2-hop-256-vs-512-criterio.md).
// Mide SOLO el render del motor, con el mismo audio en las dos filas:
// 593 frames x 512 = 1186 frames x 256 = 303.616 muestras (6,885 s a 44,1 kHz).
// No toca el plugin, no carga vocoder, no usa REAPER. Via (a): hopSize en el request.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "../Source/Audio/Synthesis/PitchcoreRequest.h"
#include "../Source/Audio/Synthesis/PsolaSynthesizer.h"
#include "pitchcore/f0.h"
#include "pitchcore/psola.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 44100.0;
constexpr int kSamples = 303616; // 593*512 == 1186*256
constexpr int kReps = 7;
constexpr int kWarmup = 2;

DspSynthesisRequest makeRequest (int hop)
{
    const int frames = kSamples / hop;
    DspSynthesisRequest r;
    r.sampleRate = static_cast<int> (kSr);
    r.hopSize = hop;
    r.numOutputFrames = frames;
    r.sourceStartSample = 0;
    r.source.resize (static_cast<size_t> (kSamples));
    for (int i = 0; i < kSamples; ++i)
    {
        const double t = static_cast<double> (i) / kSr;
        double v = 0.0;
        for (int h = 1; h <= 6; ++h)
            v += std::sin (2.0 * kPi * 220.0 * h * t) / h;
        const double am = 0.75 + 0.25 * std::sin (2.0 * kPi * 5.0 * t);
        r.source[(size_t) i] = (float) (0.2 * v * am);
    }
    r.sourceFrame.resize (static_cast<size_t> (frames));
    r.pitchRatio.assign (static_cast<size_t> (frames), 1.0f);
    r.sourceF0.assign (static_cast<size_t> (frames), 220.0f);
    r.sourceVoiced.assign (static_cast<size_t> (frames), 1);
    for (int j = 0; j < frames; ++j)
        r.sourceFrame[(size_t) j] = (double) j;
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
    Run r;
    r.ok = ok;
    r.size = out.size();
    r.err = ok ? std::string() : err;
    return r;
}

Run runPsola (const DspSynthesisRequest &req)
{
    const std::vector<float> out = PsolaSynthesizer::render (req);
    Run r;
    r.ok = ! out.empty();
    r.size = out.size();
    r.err = out.empty() ? "empty" : "";
    return r;
}

double median (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

void bench (const char *engine, int hop)
{
    const int frames = kSamples / hop;
    const DspSynthesisRequest req0 = makeRequest (hop);
    const int expected = frames * hop;
    Run last;

    for (int i = 0; i < kWarmup + kReps; ++i)
    {
        const bool warm = i < kWarmup;
        const std::clock_t c0 = std::clock();
        const auto t0 = std::chrono::steady_clock::now();
        last = std::string (engine) == "PSOLA" ? runPsola (req0) : runCore (req0);
        const auto t1 = std::chrono::steady_clock::now();
        const std::clock_t c1 = std::clock();
        const double wall = std::chrono::duration<double, std::milli> (t1 - t0).count();
        const double cpu = 1000.0 * (double) (c1 - c0) / (double) CLOCKS_PER_SEC;
        std::printf ("ROW,%s,%d,%d,%d,%s,%d,%.3f,%.3f,%.4f,%zu\n", engine, hop, frames,
                     expected, warm ? "warmup" : "rep", i, wall, cpu,
                     wall > 0.0 ? cpu / wall : 0.0, last.size);
    }
    std::printf ("%s,%s,%d,frames=%d,samples=%d,size=%zu,expected=%d%s%s\n",
                 last.ok ? "MED" : "FAIL", engine, hop, frames, expected, last.size,
                 expected, last.ok ? "" : ",err=", last.err.c_str());
}

void collect (const char *engine, int hop, std::vector<double> &wall,
              std::vector<double> &ratio)
{
    const DspSynthesisRequest req0 = makeRequest (hop);
    for (int i = 0; i < kWarmup + kReps; ++i)
    {
        const bool warm = i < kWarmup;
        const std::clock_t c0 = std::clock();
        const auto t0 = std::chrono::steady_clock::now();
        if (std::string (engine) == "PSOLA") runPsola (req0); else runCore (req0);
        const auto t1 = std::chrono::steady_clock::now();
        const std::clock_t c1 = std::clock();
        if (warm) continue;
        const double w = std::chrono::duration<double, std::milli> (t1 - t0).count();
        const double c = 1000.0 * (double) (c1 - c0) / (double) CLOCKS_PER_SEC;
        wall.push_back (w);
        ratio.push_back (w > 0.0 ? c / w : 0.0);
    }
}
} // namespace

int main (int argc, char **argv)
{
    if (argc < 2)
    {
        std::printf ("uso: hop_bench <out.csv|->\n");
        return 2;
    }
    const bool toFile = std::string (argv[1]) != "-";
    FILE *f = toFile ? std::fopen (argv[1], "w") : stdout;
    if (! f)
    {
        std::printf ("FAIL no puedo escribir %s\n", argv[1]);
        return 2;
    }
    auto out = [&](const char *fmt, auto... a) { std::fprintf (f, fmt, a...); };

    out ("engine,hop,frames,samples,phase,rep,wall_ms,cpu_ms,cpu_ratio,out_size\n");
    const int hops[2] = { 512, 256 };
    std::vector<double> w[2][2], rt[2][2];
    for (int e = 0; e < 2; ++e)
    {
        const char *engine = e == 0 ? "PSOLA" : "PITCHCORE";
        for (int h = 0; h < 2; ++h)
        {
            const int hop = hops[h];
            const int frames = kSamples / hop;
            const DspSynthesisRequest req0 = makeRequest (hop);
            Run last;
            for (int i = 0; i < kWarmup + kReps; ++i)
            {
                const bool warm = i < kWarmup;
                const std::clock_t c0 = std::clock();
                const auto t0 = std::chrono::steady_clock::now();
                last = e == 0 ? runPsola (req0) : runCore (req0);
                const auto t1 = std::chrono::steady_clock::now();
                const std::clock_t c1 = std::clock();
                const double wall =
                    std::chrono::duration<double, std::milli> (t1 - t0).count();
                const double cpu = 1000.0 * (double) (c1 - c0) / (double) CLOCKS_PER_SEC;
                out ("%s,%d,%d,%d,%s,%d,%.3f,%.3f,%.4f,%zu\n", engine, hop, frames,
                     frames * hop, warm ? "warmup" : "rep", i, wall, cpu,
                     wall > 0.0 ? cpu / wall : 0.0, last.size);
                if (! warm)
                {
                    w[e][h].push_back (wall);
                    rt[e][h].push_back (wall > 0.0 ? cpu / wall : 0.0);
                }
            }
            if (! last.ok || (int) last.size != frames * hop)
                out ("FAIL,%s,%d,size=%zu,expected=%d,err=%s\n", engine, hop, last.size,
                     frames * hop, last.err.c_str());
        }
    }

    out ("\n# resumen (2 warm-up + %d reps, mismo audio %d muestras)\n", kReps, kSamples);
    out ("engine,hop,median_ms,min_ms,max_ms,median_cpu_ratio\n");
    for (int e = 0; e < 2; ++e)
        for (int h = 0; h < 2; ++h)
        {
            auto wall = w[e][h];
            std::sort (wall.begin(), wall.end());
            out ("%s,%d,%.3f,%.3f,%.3f,%.4f\n", e == 0 ? "PSOLA" : "PITCHCORE", hops[h],
                 median (w[e][h]), wall.front(), wall.back(), median (rt[e][h]));
        }
    for (int e = 0; e < 2; ++e)
    {
        const double a = median (w[e][0]), b = median (w[e][1]);
        out ("RATIO,%s,t256/t512=%.4f\n", e == 0 ? "PSOLA" : "PITCHCORE",
             a > 0.0 ? b / a : 0.0);
    }
    if (toFile) std::fclose (f);
    return 0;
}

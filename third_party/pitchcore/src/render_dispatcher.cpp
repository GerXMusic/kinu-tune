#include "pitchcore/render_dispatcher.h"
#include "pitchcore/engine.h"
#include <algorithm>
#include <cmath>

namespace pitchcore {

std::pair<int, int> computeSynthesisRange (const RenderRequest& req,
                                          int dirtyStart, int dirtyEnd,
                                          const RenderOptions& opt)
{
    const int total = (int) req.sourceVoiced.size();
    if (total == 0 || dirtyEnd <= dirtyStart)
        return { dirtyStart, dirtyEnd };

    dirtyStart = std::max (0, dirtyStart);
    dirtyEnd = std::min (total, dirtyEnd);

    auto isVoiced = [&] (int i) {
        return i >= 0 && i < total && req.sourceVoiced[(size_t) i] != 0;
    };

    // Hacia atras: incluir los tramos voiced vecinos, PUENTEANDO los huecos
    // unvoiced cortos (asi las notas vecinas se sintetizan juntas y no hay un
    // reset de fase en la union).
    int start = dirtyStart;
    int backGap = 0;
    while (start > 0)
    {
        if (isVoiced (start - 1)) { --start; backGap = 0; continue; }
        if (backGap < opt.gapBridgeFrames) { --start; ++backGap; continue; }
        break;
    }
    // contexto: el vocoder necesita frames de contexto para estabilizar la fase
    start = std::max (0, start - opt.contextFrames);

    // Hacia delante, igual que atras
    int end = dirtyEnd;
    int fwdGap = 0;
    while (end < total)
    {
        if (isVoiced (end)) { ++end; fwdGap = 0; continue; }
        if (fwdGap < opt.gapBridgeFrames) { ++end; ++fwdGap; continue; }
        break;
    }
    end = std::min (total, end + opt.contextFrames);

    return { start, end };
}

std::vector<float> renderRange (const RenderRequest& req,
                                int dirtyStart, int dirtyEnd,
                                IEngine& engine,
                                const RenderOptions& opt)
{
    const auto rango = computeSynthesisRange (req, dirtyStart, dirtyEnd, opt);
    const int start = rango.first, end = rango.second;
    const int nFrames = end - start;
    if (nFrames <= 0) return {};

    const double hop = req.curveHop;
    const long s0 = (long) std::llround ((double) start * hop);
    const long s1 = (long) std::llround ((double) end * hop);
    if (s1 <= s0) return {};

    // --- la peticion del motor (las curvas siguen siendo absolutas) ---
    RenderRequest sub = req;
    sub.sourceStartSample = (int) s0;
    sub.numSamples = (int) (s1 - s0);
    sub.frameOffset = start;
    sub.totalFrames = (int) req.sourceVoiced.size();

    std::vector<float> sintetizado;
    std::string err;
    if (! engine.render (sub, sintetizado, err)) return {};
    if ((long) sintetizado.size() < s1 - s0) sintetizado.resize ((size_t) (s1 - s0), 0.f);

    if (! req.original) return sintetizado;

    // --- la mezcla "stability-first" ---
    // Por defecto manda lo SINTETIZADO en toda la region: el original solo se
    // devuelve en los tramos unvoiced LARGOS (respiraciones, silencios), NO en
    // los huecos cortos entre notas. Mezclar el original con lo sintetizado en
    // los ataques produce batido (dos fases distintas sumandose).
    const int total = (int) req.sourceVoiced.size();
    std::vector<float> peso ((size_t) (s1 - s0), 1.f);   // 1 = sintetizado
    if (opt.returnOriginalOnUnvoiced)
    {
        int i = 0;
        while (i < nFrames)
        {
            const int f = start + i;
            const bool voiced = (f >= 0 && f < total) && req.sourceVoiced[(size_t) f] != 0;
            if (voiced) { ++i; continue; }
            const int runStart = i;
            while (i < nFrames)
            {
                const int g = start + i;
                const bool v = (g >= 0 && g < total) && req.sourceVoiced[(size_t) g] != 0;
                if (v) break;
                ++i;
            }
            const int runLen = i - runStart;
            if (runLen >= opt.keepOriginalUnvoicedFrames)
                for (int k = runStart; k < i; ++k) peso[(size_t) k] = 0.f;
        }
    }

    // a muestras (sample-and-hold) y rampas en las transiciones
    const long n = s1 - s0;
    std::vector<float> mask (n, 1.f);
    for (int i = 0; i < nFrames; ++i)
    {
        const long a = (long) i * (long) (hop >= 1 ? hop : 1);
        const long b = std::min (n, a + (long) hop);
        for (long s = a; s < b; ++s) mask[(size_t) s] = peso[(size_t) i];
    }
    // rampas lineales centradas en el limite del frame
    const long ramp = std::max ((long) opt.rampSamples, (long) hop * 2);
    for (int i = 0; i < nFrames - 1; ++i)
    {
        if (peso[(size_t) i] == peso[(size_t) i + 1]) continue;
        const long centre = (long) (i + 1) * (long) hop;
        const long ra = std::max (0L, centre - ramp / 2);
        const long rb = std::min (n, centre + ramp / 2);
        const float from = peso[(size_t) i], to = peso[(size_t) i + 1];
        for (long s = ra; s < rb; ++s)
        {
            const float t = (float) (s - ra) / (float) std::max (1L, rb - ra);
            mask[(size_t) s] = from + (to - from) * t;
        }
    }

    // mezclar
    std::vector<float> out ((size_t) n, 0.f);
    for (long s = 0; s < n; ++s)
    {
        const long os = s0 + s;
        double oD = 0.0;
        readOriginal (req, os, oD);
        const float o = (float) oD;
        const float m = mask[(size_t) s];
        out[(size_t) s] = sintetizado[(size_t) s] * m + o * (1.f - m);
    }
    return out;
}

} // namespace pitchcore

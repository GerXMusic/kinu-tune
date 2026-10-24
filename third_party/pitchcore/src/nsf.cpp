#include "pitchcore/nsf.h"
#include <algorithm>
#include <cmath>

namespace pitchcore {

namespace {

// --- Escala mel Slaney (librosa, htk=False) ---
constexpr double kFminSlaney = 0.0;
constexpr double kFspSlaney   = 200.0 / 3.0;   // paso de la zona lineal
constexpr double kMinLogHz   = 1000.0;

// logstep = log(6.4)/27.0 (librosa). Se calcula en runtime porque std::log no es
// constexpr en C++17, y una constante escrita a mano es facil equivocarse.
inline double melLogStep() { static const double v = std::log(6.4) / 27.0; return v; }

double hzToMel (double f)
{
    double m = (f - kFminSlaney) / kFspSlaney;
    if (f >= kMinLogHz)
        m = ((kMinLogHz - kFminSlaney) / kFspSlaney) + std::log(f / kMinLogHz) / melLogStep();
    return m;
}

double melToHz (double m)
{
    double f = kFminSlaney + kFspSlaney * m;
    const double minLogMel = (kMinLogHz - kFminSlaney) / kFspSlaney;
    if (m >= minLogMel)
        f = kMinLogHz * std::exp(melLogStep() * (m - minLogMel));
    return f;
}

} // namespace

std::vector<std::vector<float>> nsfMelFilterbank (const NsfMelParams& p)
{
    const int nBins = p.nFft / 2 + 1;
    const int nPts  = p.nMels + 2;

    // Puntos en Hz de los nMels+2 cortes del filtro (equiespaciados en mel).
    std::vector<double> melF (nPts);
    {
        const double mMin = hzToMel (p.fmin);
        const double mMax = hzToMel (p.fmax);
        for (int i = 0; i < nPts; ++i)
            melF[(size_t) i] = melToHz (mMin + (mMax - mMin) * double (i) / double (nPts - 1));
    }

    // Frecuencias de cada bin de la FFT.
    std::vector<double> fftFreq (nBins);
    for (int k = 0; k < nBins; ++k)
        fftFreq[(size_t) k] = double (k) * double (p.sampleRate) / double (p.nFft);

    std::vector<std::vector<float>> fb ((size_t) p.nMels, std::vector<float> ((size_t) nBins, 0.f));

    for (int i = 0; i < p.nMels; ++i)
    {
        const double fdiff = melF[(size_t) i + 1] - melF[(size_t) i];
        const double fdiffNext = melF[(size_t) i + 2] - melF[(size_t) i + 1];
        if (fdiff <= 0.0 || fdiffNext <= 0.0) continue;
        for (int k = 0; k < nBins; ++k)
        {
            // rampas triangulares de librosa:
            //   lower = (fftfreqs[k] - mel_f[i])   / (mel_f[i+1] - mel_f[i])
            //   upper = (mel_f[i+2] - fftfreqs[k]) / (mel_f[i+2] - mel_f[i+1])
            const double lower = (fftFreq[(size_t) k] - melF[(size_t) i]) / fdiff;
            const double upper = (melF[(size_t) i + 2] - fftFreq[(size_t) k]) / fdiffNext;
            const double v = std::max (0.0, std::min (lower, upper));
            fb[(size_t) i][(size_t) k] = (float) v;
        }
        // Normalizacion de area Slaney: 2.0 / ancho del filtro.
        const double enorm = 2.0 / (melF[(size_t) i + 2] - melF[(size_t) i]);
        for (int k = 0; k < nBins; ++k)
            fb[(size_t) i][(size_t) k] = (float) (fb[(size_t) i][(size_t) k] * enorm);
    }
    return fb;
}

std::vector<float> nsfStftMagnitude (const std::vector<float>& x, const NsfMelParams& p,
                                     int& nFrames)
{
    nFrames = 0;
    const int left  = (p.winSize - p.hopSize) / 2;
    const int right = (p.winSize - p.hopSize + 1) / 2;
    const int n = (int) x.size();

    // reflect-pad (como torch.nn.functional.pad con mode='reflect'):
    //   izquierda: x[left], x[left-1], ... x[1]
    //   derecha:   x[n-2], x[n-3], ... x[n-1-right]
    std::vector<double> y ((size_t) (n + left + right), 0.0);
    for (int i = 0; i < n; ++i) y[(size_t) (i + left)] = x[(size_t) i];
    for (int i = 0; i < left; ++i) {
        const int src = left - i;
        y[(size_t) i] = (src >= 0 && src < n) ? x[(size_t) src] : 0.0;
    }
    for (int i = 0; i < right; ++i) {
        const int src = n - 2 - i;
        const int dst = n + left + i;
        y[(size_t) dst] = (src >= 0 && src < n) ? x[(size_t) src] : 0.0;
    }
    const int ny = n + left + right;

    const int nBins = p.nFft / 2 + 1;
    nFrames = (ny >= p.nFft) ? 1 + (ny - p.nFft) / p.hopSize : 0;
    if (nFrames <= 0) return {};

    // ventana hann periodica
    std::vector<double> win ((size_t) p.winSize);
    for (int i = 0; i < p.winSize; ++i)
        win[(size_t) i] = 0.5 * (1.0 - std::cos (2.0 * 3.14159265358979323846 * double (i) / double (p.winSize)));

    std::vector<float> out ((size_t) nFrames * (size_t) nBins, 0.f);
    std::vector<double> re ((size_t) p.nFft, 0.0), im ((size_t) p.nFft, 0.0);

    for (int t = 0; t < nFrames; ++t)
    {
        const int off = t * p.hopSize;
        for (int i = 0; i < p.nFft; ++i)
        {
            re[(size_t) i] = y[(size_t) (off + i)] * win[(size_t) i];
            im[(size_t) i] = 0.0;
        }
        // DFT real de nFft puntos (nFft es potencia de 2 -> radix-2)
        int n = p.nFft;
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap (re[(size_t) i], re[(size_t) j]); std::swap (im[(size_t) i], im[(size_t) j]); }
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const double ang = -2.0 * 3.14159265358979323846 / double (len);
            const double wr = std::cos (ang), wi = std::sin (ang);
            const int half = len / 2;
            for (int i = 0; i < n; i += len)
            {
                // el factor raiz w^k, que avanza en cada paso de la mariposa
                double cr = 1.0, ci = 0.0;
                for (int k = 0; k < half; ++k)
                {
                    const double ur = re[(size_t) (i + k)],      ui = im[(size_t) (i + k)];
                    const double vr = re[(size_t) (i + k + half)], vi = im[(size_t) (i + k + half)];
                    const double tr = vr * cr - vi * ci;
                    const double ti = vr * ci + vi * cr;
                    re[(size_t) (i + k)] = ur + tr; im[(size_t) (i + k)] = ui + ti;
                    re[(size_t) (i + k + half)] = ur - tr; im[(size_t) (i + k + half)] = ui - ti;
                    const double nr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;
                    cr = nr;
                }
            }
        }
        for (int k = 0; k < nBins; ++k)
        {
            const double mag = std::sqrt (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]);
            out[(size_t) t * (size_t) nBins + (size_t) k] = (float) mag;
        }
    }
    return out;
}

std::vector<float> nsfComputeMel (const std::vector<float>& x, const NsfMelParams& p,
                                  int& nFrames)
{
    nFrames = 0;
    const auto spec = nsfStftMagnitude (x, p, nFrames);
    if (nFrames <= 0) return {};
    const int nBins = p.nFft / 2 + 1;
    const auto fb = nsfMelFilterbank (p);

    std::vector<float> mel ((size_t) nFrames * (size_t) p.nMels, 0.f);
    for (int t = 0; t < nFrames; ++t)
        for (int m = 0; m < p.nMels; ++m)
        {
            double acc = 0.0;
            const auto& filt = fb[(size_t) m];
            for (int k = 0; k < nBins; ++k)
                acc += double (filt[(size_t) k]) * double (spec[(size_t) t * (size_t) nBins + (size_t) k]);
            mel[(size_t) t * (size_t) p.nMels + (size_t) m] = (float) std::log (std::max (acc, p.clipVal));
        }
    return mel;
}

std::vector<int> nsfFrameCenters (int nSamples, const NsfMelParams& p)
{
    const int left = (p.winSize - p.hopSize) / 2;
    const int ny = nSamples + 2 * left;
    const int nFrames = (ny >= p.nFft) ? 1 + (ny - p.nFft) / p.hopSize : 0;
    std::vector<int> c ((size_t) std::max (0, nFrames));
    for (int t = 0; t < nFrames; ++t)
        c[(size_t) t] = t * p.hopSize + (p.nFft / 2 - left);
    return c;
}

std::vector<float> nsfResampleF0 (const std::vector<float>& f0PerSample, int nSamples,
                                  const NsfMelParams& p, int& nFrames)
{
    nFrames = 0;
    const auto centers = nsfFrameCenters (nSamples, p);
    nFrames = (int) centers.size();
    std::vector<float> out (centers.size(), 0.f);
    if (centers.empty() || nSamples <= 0) return out;
    for (size_t t = 0; t < centers.size(); ++t)
    {
        const double pos = double (centers[t]);
        const long i0 = (long) std::floor (pos);
        const long i1 = std::min<long> (nSamples - 1, i0 + 1);
        const double fr = pos - double (i0);
        const double a = (i0 >= 0 && i0 < nSamples) ? double (f0PerSample[(size_t) i0]) : 0.0;
        const double b = (i1 >= 0 && i1 < nSamples) ? double (f0PerSample[(size_t) i1]) : 0.0;
        out[t] = (float) (a + (b - a) * fr);
    }
    return out;
}

std::vector<float> nsfContinuousF0 (const std::vector<float>& f0, const std::vector<char>& voiced,
                                   int maxGapFrames)
{
    const int n = (int) f0.size();
    std::vector<float> out (f0);
    if (n == 0) return out;

    // indices voiced
    std::vector<int> idx;
    idx.reserve ((size_t) n);
    for (int i = 0; i < n; ++i) if (voiced[(size_t) i]) idx.push_back (i);
    if (idx.empty()) return out;

    for (int i = 0; i < n; ++i)
    {
        if (voiced[(size_t) i]) continue;
        // vecinos voiced mas cercanos
        auto itLo = std::lower_bound (idx.begin(), idx.end(), i);
        int lo = -1, hi = -1;
        if (itLo != idx.end()) hi = *itLo;
        if (itLo != idx.begin()) lo = *std::prev (itLo);
        if (lo < 0 || hi < 0) continue;                 // fuera de rango voiced
        if (hi - lo > maxGapFrames) continue;           // hueco largo: silencio real
        const double u = double (i - lo) / double (hi - lo);
        out[(size_t) i] = (float) (f0[(size_t) lo] + (f0[(size_t) hi] - f0[(size_t) lo]) * u);
    }
    return out;
}

} // namespace pitchcore

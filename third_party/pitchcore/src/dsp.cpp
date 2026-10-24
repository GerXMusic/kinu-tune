#include "pitchcore/dsp.h"
#include <algorithm>
#include <cmath>

namespace pitchcore {

// ---------------------------------------------------------------------------
// Shelf de agudos
// ---------------------------------------------------------------------------
void airShelf (std::vector<float>& x, double sampleRate, double gainDb, double f0)
{
    if (x.empty() || gainDb == 0.0 || sampleRate <= 0) return;

    const double A = std::pow (10.0, gainDb / 20.0);
    const double w0 = 2.0 * 3.14159265358979323846 * f0 / sampleRate;
    if (w0 <= 0 || w0 >= 3.14159265358979323846) return;   // fuera de banda
    const double cw = std::cos (w0), sw = std::sin (w0);
    const double alpha = sw / (2.0 * 1.4142135623730951);   // Butterworth Q = 1/sqrt2

    // paso bajo Butterworth de 2º orden (forma directa II transpuesta)
    double b0 = (1.0 - cw) / 2.0, b1 = 1.0 - cw, b2 = (1.0 - cw) / 2.0;
    double a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha;
    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;

    double s1 = 0.0, s2 = 0.0;
    for (float& v : x)
    {
        const double xn = v;
        const double lp = b0 * xn + s1;
        s1 = b1 * xn - a1 * lp + s2;
        s2 = b2 * xn - a2 * lp;
        v = (float) (lp + A * (xn - lp));
    }
}

// ---------------------------------------------------------------------------
// FFT radix-2
// ---------------------------------------------------------------------------
namespace {

bool esPotenciaDeDos (int n) { return n > 0 && (n & (n - 1)) == 0; }

} // namespace

Fft::Fft (int n) : n_ (n)
{
    if (! esPotenciaDeDos (n)) n_ = 1;
    rev_.resize ((size_t) n_);
    int lg = 0;
    while ((1 << lg) < n_) ++lg;
    for (int i = 0; i < n_; ++i)
    {
        int r = 0;
        for (int b = 0; b < lg; ++b) if (i & (1 << b)) r |= 1 << (lg - 1 - b);
        rev_[(size_t) i] = r;
    }
    roots_.resize ((size_t) n_);
    const double th = -2.0 * 3.14159265358979323846 / (double) n_;
    for (int i = 0; i < n_; ++i)
    {
        const double a = th * i;
        roots_[(size_t) i] = std::complex<float> ((float) std::cos (a), (float) std::sin (a));
    }
}

void Fft::forward (std::vector<std::complex<float>>& a) const
{
    const int n = n_;
    if ((int) a.size() < n) return;
    for (int i = 0; i < n; ++i)
        if (i < rev_[(size_t) i]) std::swap (a[(size_t) i], a[(size_t) rev_[(size_t) i]]);
    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len >> 1;
        const int step = n / len;
        for (int i = 0; i < n; i += len)
        {
            for (int j = 0; j < half; ++j)
            {
                const std::complex<float> u = a[(size_t) (i + j)];
                const std::complex<float> v = a[(size_t) (i + j + half)] * roots_[(size_t) (j * step)];
                a[(size_t) (i + j)] = u + v;
                a[(size_t) (i + j + half)] = u - v;
            }
        }
    }
}

void Fft::inverse (std::vector<std::complex<float>>& a) const
{
    const int n = n_;
    if ((int) a.size() < n) return;
    for (int i = 0; i < n; ++i)
        if (i < rev_[(size_t) i]) std::swap (a[(size_t) i], a[(size_t) rev_[(size_t) i]]);
    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len >> 1;
        const int step = n / len;
        for (int i = 0; i < n; i += len)
        {
            for (int j = 0; j < half; ++j)
            {
                const std::complex<float> u = a[(size_t) (i + j)];
                // la inversa usa las raices inversas (conjugadas): conj(root^k)
                const std::complex<float> v = a[(size_t) (i + j + half)]
                                          * std::conj (roots_[(size_t) ((j * step) % n)]);
                a[(size_t) (i + j)] = u + v;
                a[(size_t) (i + j + half)] = u - v;
            }
        }
    }
    const float inv = 1.0f / (float) n;
    for (int i = 0; i < n; ++i) a[(size_t) i] *= inv;
}

// ---------------------------------------------------------------------------
// STFT / ISTFT
// ---------------------------------------------------------------------------
void stftComplex (const std::vector<float>& x, int nFft, int hop, int& nFrames,
                  std::vector<std::complex<float>>& out)
{
    out.clear();
    nFrames = 0;
    if (nFft < 4 || hop < 1 || (int) x.size() < nFft) return;

    const Fft fft (nFft);
    const int bins = nFft / 2 + 1;
    nFrames = 1 + ((int) x.size() - nFft) / hop;
    out.resize ((size_t) nFrames * (size_t) bins);

    // ventana de Hann simetrica (como numpy.hanning)
    std::vector<float> win ((size_t) nFft);
    for (int i = 0; i < nFft; ++i)
        win[(size_t) i] = (float) (0.5 * (1.0 - std::cos (2.0 * 3.14159265358979323846 * i / (nFft - 1))));

    std::vector<std::complex<float>> buf ((size_t) nFft);
    for (int f = 0; f < nFrames; ++f)
    {
        const int off = f * hop;
        for (int i = 0; i < nFft; ++i)
            buf[(size_t) i] = std::complex<float> (x[(size_t) (off + i)] * win[(size_t) i], 0.f);
        fft.forward (buf);
        for (int b = 0; b < bins; ++b)
            out[(size_t) f * (size_t) bins + (size_t) b] = buf[(size_t) b];
    }
}

void istftComplex (const std::vector<std::complex<float>>& spec, int nFft, int hop,
                   int nFrames, std::vector<float>& out)
{
    out.clear();
    if (nFft < 4 || hop < 1 || nFrames <= 0) return;
    const int bins = nFft / 2 + 1;
    if ((int) spec.size() < nFrames * bins) return;

    const Fft fft (nFft);
    std::vector<float> win ((size_t) nFft);
    for (int i = 0; i < nFft; ++i)
        win[(size_t) i] = (float) (0.5 * (1.0 - std::cos (2.0 * 3.14159265358979323846 * i / (nFft - 1))));

    const int largo = (nFrames - 1) * hop + nFft;
    std::vector<double> acc ((size_t) largo, 0.0);
    std::vector<double> wsum ((size_t) largo, 0.0);
    std::vector<std::complex<float>> buf ((size_t) nFft);

    for (int f = 0; f < nFrames; ++f)
    {
        const int off = f * hop;
        std::fill (buf.begin(), buf.end(), std::complex<float> (0.f, 0.f));
        for (int b = 0; b < bins; ++b)
            buf[(size_t) b] = spec[(size_t) f * (size_t) bins + (size_t) b];
        // reconstruir el espectro conjugado
        for (int b = bins; b < nFft - 1; ++b)
            buf[(size_t) b] = std::conj (spec[(size_t) f * (size_t) bins + (size_t) (nFft - b)]);
        if (nFft % 2 == 0) buf[(size_t) (nFft / 2)] = std::complex<float> (buf[(size_t) (nFft / 2)].real(), 0.f);
        fft.inverse (buf);
        for (int i = 0; i < nFft; ++i)
        {
            const double w = win[(size_t) i];
            acc[(size_t) (off + i)] += buf[(size_t) i].real() * w;
            wsum[(size_t) (off + i)] += w * w;
        }
    }
    out.resize ((size_t) largo);
    for (int i = 0; i < largo; ++i)
        out[(size_t) i] = (float) (wsum[(size_t) i] > 1e-8 ? acc[(size_t) i] / wsum[(size_t) i] : 0.0);
}

} // namespace pitchcore

#include "pitchcore/resample.h"
#include <algorithm>
#include <cmath>

namespace pitchcore {

namespace {

constexpr double kPi = 3.14159265358979323846;

// I0(x): funcion de Bessel modificada de primer tipo, orden 0 (serie).
double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    const double h = x * 0.5;
    for (int k = 1; k < 40; ++k)
    {
        term *= (h / k) * (h / k);
        sum += term;
        if (term < sum * 1e-16) break;
    }
    return sum;
}

// Ventana de Kaiser: beta 8.6 (attenuacion ~ -70 dB).
double kaiser (double r, double beta)
{
    if (r >= 1.0) return 0.0;
    const double a = beta * std::sqrt (1.0 - r * r);
    return besselI0 (a) / besselI0 (beta);
}

} // namespace

void Resampler::init (double srcRate, double dstRate)
{
    ready_ = false;
    if (srcRate <= 0.0 || dstRate <= 0.0) return;

    srcRate_ = srcRate;
    dstRate_ = dstRate;
    ratio_ = dstRate / srcRate;
    // Nyquist de la tasa mas baja, normalizado a la tasa de entrada.
    cutoff_ = 0.5 * std::min (1.0, ratio_);

    half_ = 16;
    taps_ = 2 * half_;
    center_ = half_ - 1;
    phases_ = 1024;
    table_.assign ((size_t) phases_ * (size_t) taps_, 0.f);

    for (int p = 0; p < phases_; ++p)
    {
        const double frac = double (p) / double (phases_);
        for (int j = 0; j < taps_; ++j)
        {
            // distancia (en muestras de entrada) de x[base - center + j] a srcPos
            // (que esta en base + frac):
            const double t = double (j - center_) - frac;
            // kernel: cutoff*sinc(cutoff*t) = sin(pi*cutoff*t)/(pi*t)
            // OJO: el cutoff ya va DENTRO del seno; multiplicar otra vez por
            // cutoff daria una ganancia de cutoff^2 (y una senal distortionada
            // con la energia conservada, que es lo que pasaba).
            double s;
            if (std::fabs (t) < 1e-12) s = cutoff_;
            else s = std::sin (kPi * cutoff_ * t) / (kPi * t);
            // ventana de Kaiser centrada en el centro del kernel (igual para todas
            // las fases, para que la respuesta al impulso sea constante)
            const double r = std::fabs (double (j - center_)) / (double (half_) + 0.5);
            table_[(size_t) p * (size_t) taps_ + (size_t) j] = (float) (s * kaiser (r, 8.6));
        }
    }
    ready_ = true;
}

void Resampler::process (const std::vector<float>& in, std::vector<float>& out) const
{
    if (! ready_ || in.empty()) { out.clear(); return; }

    const double nOutD = double (in.size()) * ratio_;
    const long nOut = (long) std::floor (nOutD);
    out.assign ((size_t) std::max (0L, nOut), 0.f);

    const long nIn = (long) in.size();
    for (long n = 0; n < nOut; ++n)
    {
        const double srcPos = (double (n) + 0.5) / ratio_ - 0.5;
        const long base = (long) std::floor (srcPos);
        const double frac = srcPos - double (base);
        int p = (int) (frac * double (phases_));
        if (p >= phases_) p = phases_ - 1;
        if (p < 0) p = 0;
        const float* h = &table_[(size_t) p * (size_t) taps_];

        double acc = 0.0;
        const long start = base - (half_ - 1);
        for (int j = 0; j < taps_; ++j)
        {
            const long i = start + j;
            if (i < 0 || i >= nIn) continue;
            acc += double (in[(size_t) i]) * double (h[j]);
        }
        out[(size_t) n] = (float) acc;
    }
}

} // namespace pitchcore

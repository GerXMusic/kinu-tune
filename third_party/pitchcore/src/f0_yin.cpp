#include "pitchcore/f0.h"
#include <algorithm>
#include <cmath>

namespace pitchcore {

std::vector<F0Frame> estimateF0(const std::vector<float>& x, const F0Params& p)
{
    std::vector<F0Frame> out;
    const int W      = std::max(64, (int) std::lround(p.windowSec * p.sampleRate));
    const int hop    = std::max(1,  (int) std::lround(p.hopSec * p.sampleRate));
    const int tauMin = std::max(2,  (int) std::floor(p.sampleRate / p.fMax));
    const int tauMax = std::min(W,  (int) std::ceil (p.sampleRate / p.fMin));
    const int need   = W + tauMax + 2;
    if ((int) x.size() < need) return out;

    std::vector<double> d(tauMax + 2), cm(tauMax + 2);

    for (long start = 0; start + need <= (long) x.size(); start += hop) {
        const float* s = x.data() + start;

        double e = 0.0;
        for (int j = 0; j < W; ++j) e += (double) s[j] * s[j];
        F0Frame f;
        f.timeSec = (start + W * 0.5) / p.sampleRate;
        f.rmsDb   = 10.0 * std::log10(e / W + 1e-20);

        if (f.rmsDb < p.silenceDb) { out.push_back(f); continue; }

        // Función de diferencia (O(W * tauMax): suficiente offline; sustituible por FFT)
        d[0] = 0.0;
        for (int tau = 1; tau <= tauMax + 1; ++tau) {
            double sum = 0.0;
            for (int j = 0; j < W; ++j) { const double dv = (double) s[j] - s[j + tau]; sum += dv * dv; }
            d[tau] = sum;
        }
        // Diferencia media acumulada normalizada
        cm[0] = 1.0;
        double run = 0.0;
        for (int tau = 1; tau <= tauMax + 1; ++tau) {
            run += d[tau];
            cm[tau] = run > 1e-20 ? d[tau] * tau / run : 1.0;
        }

        int best = -1;
        for (int tau = tauMin; tau <= tauMax; ++tau) {
            if (cm[tau] < p.yinThresh) {
                while (tau + 1 <= tauMax && cm[tau + 1] < cm[tau]) ++tau;
                best = tau; break;
            }
        }
        if (best < 0) { // sin cruce de umbral: mínimo global (queda unvoiced si aper alta)
            best = tauMin;
            for (int tau = tauMin; tau <= tauMax; ++tau) if (cm[tau] < cm[best]) best = tau;
        }
        f.aper = cm[best];

        double tauEst = best; // interpolación parabólica
        if (best > 1 && best < tauMax) {
            const double a = cm[best - 1], b = cm[best], c = cm[best + 1];
            const double den = a - 2 * b + c;
            if (std::fabs(den) > 1e-12) tauEst = best + 0.5 * (a - c) / den;
        }
        f.voiced = f.aper < p.yinThresh;
        f.f0Hz   = p.sampleRate / tauEst;   // se anula despues si no es voiced
        // La interpolacion parabolica puede devolver un periodo por debajo de
        // tauMin, y entonces la F0 sale por encima de fMax (medido: 1030 Hz con
        // fMax = 1000). Es una estimacion invalida: esas notas agudas son las
        // que el motor cantaba con un glissando. Se recorta al rango valido.
        if (f.f0Hz > p.fMax) f.f0Hz = p.fMax;
        if (f.f0Hz < p.fMin) f.f0Hz = p.fMin;
        out.push_back(f);
    }

    // --- Segunda pasada: suavizado de la aperiodicidad y decision voiced ---
    // La decision se toma sobre la MEDIANA de la aperiodicidad en una ventana
    // de `aperSmoothFrames` frames. Asi un frame con aper 0,153 en mitad de una
    // nota con aper real 0,01 deja de partirla, y las fronteras de nota (donde
    // la aperiodicidad sube de verdad) se conservan.
    const int win = std::max(1, p.aperSmoothFrames);
    if (! out.empty())
    {
        const int n = (int) out.size();
        std::vector<double> sm (out.size());
        for (int i = 0; i < n; ++i) {
            const int lo = std::max(0, i - win / 2), hi = std::min(n - 1, i + win / 2);
            std::vector<double> w;
            w.reserve((size_t)(hi - lo + 1));
            for (int k = lo; k <= hi; ++k) w.push_back(out[(size_t) k].aper);
            const int mid = (int) (w.size() / 2);
            std::nth_element(w.begin(), w.begin() + mid, w.end());
            sm[(size_t) i] = w[mid];
        }
        for (int i = 0; i < n; ++i) {
            auto& f = out[(size_t) i];
            f.aper = sm[(size_t) i];
            f.voiced = (f.aper < p.yinThresh);
            if (! f.voiced) f.f0Hz = 0.0;
        }
    }
    return out;
}

} // namespace pitchcore

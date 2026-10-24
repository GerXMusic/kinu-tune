#include "pitchcore/pitch_curve.h"
#include <algorithm>
#include <cmath>

namespace pitchcore {

namespace {

// M_PI no existe en MSVC: constante propia
constexpr double kPi = 3.14159265358979323846;

double mediana (std::vector<double> v)
{
    if (v.empty()) return 0.0;
    const size_t m = v.size() / 2;
    std::nth_element (v.begin(), v.begin() + (long) m, v.end());
    return v[m];
}

double interpLog (double f0, double f1, double u)
{
    if (f0 <= 0) return f1;
    if (f1 <= 0) return f0;
    const double a = std::log2 (f0), b = std::log2 (f1);
    return std::pow (2.0, a + (b - a) * u);
}

// El kernel coseno de la suavizacion (una ventana coseno sobre smoothSec).
std::vector<double> cosineKernel (double smoothSec, double hopSec)
{
    const int n = std::max (3, (int) std::lround (smoothSec / std::max (1e-6, hopSec)));
    std::vector<double> k ((size_t) n, 0.0);
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) (i - n / 2) * hopSec;
        k[(size_t) i] = std::cos (kPi * t / std::max (1e-9, smoothSec));
        sum += k[(size_t) i];
    }
    if (std::fabs (sum) > 1e-9)
        for (auto& v : k) v /= sum;
    return k;
}

} // namespace

void repairSource (std::vector<float>& f0, std::vector<char>& voiced,
                   const std::vector<double>& timeSec,
                   const PitchCurveParams& p)
{
    const int n = (int) f0.size();
    if (n < 5) return;
    const int half = std::max (1, p.localWinFrames / 2);

    // 1) los frames atipicos (saltos de octava) frente a la mediana local
    std::vector<char> malo ((size_t) n, 0);
    std::vector<double> w;
    w.reserve ((size_t) (2 * half + 1));
    for (int i = 0; i < n; ++i)
    {
        if (! voiced[(size_t) i]) continue;
        w.clear();
        const int lo = std::max (0, i - half), hi = std::min (n - 1, i + half);
        for (int k = lo; k <= hi; ++k)
            if (voiced[(size_t) k] && f0[(size_t) k] > 0) w.push_back (f0[(size_t) k]);
        if (w.size() < 5) continue;
        const double m = mediana (w);
        if (m <= 0) continue;
        if (std::fabs (1200.0 * std::log2 (f0[(size_t) i] / m)) > p.octaveGuardCents)
            malo[(size_t) i] = 1;
    }

    // 2) los tramos de frames malos: se reparan (interpolando) si son cortos
    int ini = -1;
    for (int i = 0; i <= n; ++i)
    {
        const bool esMalo = (i < n) && malo[(size_t) i];
        if (esMalo && ini < 0) ini = i;
        if (! esMalo && ini >= 0)
        {
            const int fin = i - 1;
            const int len = fin - ini + 1;
            if (len <= p.outlierMaxFrames)
            {
                int a = ini - 1, b = fin + 1;
                while (a >= 0 && ! voiced[(size_t) a]) --a;
                while (b < n && ! voiced[(size_t) b]) ++b;
                const double fa = (a >= 0) ? f0[(size_t) a] : 0.0;
                const double fb = (b < n) ? f0[(size_t) b] : 0.0;
                for (int k = ini; k <= fin; ++k)
                {
                    const double u = (len > 1) ? (double) (k - ini) / (len - 1) : 0.5;
                    f0[(size_t) k] = (float) interpLog (fa, fb, u);
                    voiced[(size_t) k] = 1;
                }
            }
            ini = -1;
        }
    }
}

PitchCurve buildPitchCurve (const std::vector<F0Frame>& frames,
                           const std::vector<Note>& notes,
                           const std::vector<double>& shiftCents,
                           const PitchCurveParams& p)
{
    PitchCurve c;
    const int n = (int) frames.size();
    c.f0Hz.assign ((size_t) n, 0.f);
    c.voiced.assign ((size_t) n, 0);
    c.sourceHz.assign ((size_t) n, 0.f);
    c.sourceVoiced.assign ((size_t) n, 0);
    c.timeSec.assign ((size_t) n, 0.0);
    if (n == 0) return c;

    const double hop = (n > 1) ? (frames[1].timeSec - frames[0].timeSec) : 0.003;
    for (int i = 0; i < n; ++i) c.timeSec[(size_t) i] = frames[(size_t) i].timeSec;

    // --- la fuente: el F0 del tracker, reparado ---
    for (int i = 0; i < n; ++i)
    {
        if (frames[(size_t) i].voiced && frames[(size_t) i].f0Hz > 0)
        {
            c.sourceHz[(size_t) i] = (float) frames[(size_t) i].f0Hz;
            c.sourceVoiced[(size_t) i] = 1;
        }
    }
    repairSource (c.sourceHz, c.sourceVoiced, c.timeSec, p);

    // --- la base: el SHIFT por nota (escalon, cambio en el punto medio) ---
    //
    // D-030/D-053: la curva objetivo NO es la altura cuantizada de la nota
    // (eso borraria el vibrato y los glides del cantante); es el CONTORNO de
    // la fuente desplazado por el shift de cada nota. El shift es un escalon
    // por nota que se suaviza con el kernel coseno.
    struct Seg { double t0, t1; double cents; };
    std::vector<Seg> segs;
    for (size_t k = 0; k < notes.size(); ++k)
    {
        const Note& nt = notes[k];
        if (nt.t1 <= nt.t0) continue;
        const double shift = (k < shiftCents.size()) ? shiftCents[k] : 0.0;
        segs.push_back ({ nt.t0, nt.t1, shift });
    }
    std::sort (segs.begin(), segs.end(),
               [] (const Seg& a, const Seg& b) { return a.t0 < b.t0; });

    // El escalon cubre TODA la linea de tiempo (cada nota se extiende hasta
    // el punto medio con su vecina). Si se dejara fuera de las notas un 0, la
    // suavizacion promediaria con ese 0 y el shift se hundiria en los bordes
    // de cada nota (un oido a "garganteo" en el ataque).
    std::vector<double> base ((size_t) n, 0.0);
    if (! segs.empty())
    {
        const size_t ns = segs.size();
        std::vector<double> lim (ns + 1, 0.0);
        lim[0] = -1e18;
        for (size_t k = 1; k < ns; ++k) lim[k] = 0.5 * (segs[k - 1].t1 + segs[k].t0);
        lim[ns] = 1e18;
        size_t k = 0;
        for (int i = 0; i < n; ++i)
        {
            const double t = c.timeSec[(size_t) i];
            while (k + 1 < ns && t >= lim[k + 1]) ++k;
            base[(size_t) i] = segs[k].cents;
        }
    }

    // --- suavizar el escalon con el kernel coseno ---
    const std::vector<double> kern = cosineKernel (p.smoothSec, hop);
    const int kh = (int) kern.size() / 2;
    std::vector<double> smooth ((size_t) n, 0.0);
    for (int i = 0; i < n; ++i)
    {
        double acc = 0.0;
        for (int j = 0; j < (int) kern.size(); ++j)
        {
            const int idx = std::max (0, std::min (n - 1, i - kh + j));
            acc += base[(size_t) idx] * kern[(size_t) j];
        }
        smooth[(size_t) i] = acc;
    }

    // --- la curva final: el contorno de la fuente desplazado por el shift ---
    for (int i = 0; i < n; ++i)
    {
        if (! c.sourceVoiced[(size_t) i]) continue;
        c.voiced[(size_t) i] = 1;
        c.f0Hz[(size_t) i] = (float) (c.sourceHz[(size_t) i] *
                                      std::pow (2.0, smooth[(size_t) i] / 1200.0));
    }
    return c;
}

} // namespace pitchcore

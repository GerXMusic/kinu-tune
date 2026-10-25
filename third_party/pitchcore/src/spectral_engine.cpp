#include "pitchcore/spectral_engine.h"
#include "pitchcore/dsp.h"
#include <algorithm>
#include <cmath>

// La amplitud de la armonica se corrige con el COCIENTE de la envolvente
// (los formantes): asi los formantes NO se mueven con la altura, que es lo
// que distingue a un motor de correccion de un cambiador de tono clasico.
// (con la amplitud integra, los formantes se desplazan con la altura: el
//  "efecto chipmunk" del PSOLA).
static const bool P_ENV_PRESERVE_FORMANTS = false;

namespace pitchcore {

namespace {

// El valor del espectro en una posicion FRACCIONAL (interpolacion lineal).
double interpEspec (const std::vector<float>& mags, double pos)
{
    const int n = (int) mags.size();
    if (pos < 0.0 || pos > (double) (n - 1)) return 0.0;
    const int i = (int) std::floor (pos);
    const double u = pos - (double) i;
    if (i + 1 >= n) return mags[(size_t) i];
    return (double) mags[(size_t) i] * (1.0 - u) + (double) mags[(size_t) (i + 1)] * u;
}

// La FORMA de una armonica (el pico y su leakage) evaluada en un
// desplazamiento fraccional respecto de su centro. La forma va NORMALIZADA
// (el pico vale 1) y se guarda como una tabla de 2*kShape+1 valores.
//
// Es lo que permite mover la armonica a una posicion fraccional sin diluir el
// pico: si el pico se reparte entre dos bins, pierde 6 dB.
double interpForma (const std::vector<float>& forma, double offset, int kShape)
{
    const int n = (int) forma.size();
    const double x = offset + kShape;         // en indices de la forma
    if (x <= 0.0) return (double) forma[0];
    if (x >= (double) (n - 1)) return (double) forma[(size_t) (n - 1)];
    const int k = (int) std::floor (x);
    const double u = x - (double) k;
    return (double) forma[(size_t) k] * (1.0 - u) + (double) forma[(size_t) (k + 1)] * u;
}

// Envolvente trazada por las propias armonicas (no por percentiles de banda).
//
// POR QUE: el percentil de la magnitud en una banda cae al SUELO DE RUIDO
// (medido: 0,34 donde la armonica vale 14,3) -> no son los formantes, y la
// relacion fuente/envolvente se disparaba (el HF salia x2, el LSD a 0,22).
//
// La envolvente correcta sale de las amplitudes de las armonicas: se ajusta una
// curva suave (en log2 f) que pasa por sus picos. Con eso:
//   - sin correccion, la amplitud nueva = A_m * E(m*f0)/E(m*f0) = A_m -> el
//     espectro se conserva EXACTO (el motor es transparente);
//   - con correccion, la amplitud nueva = A_m * E(m*f0_nuevo)/E(m*f0) -> el
//     cociente de la envolvente (los formantes) NO cambia con la altura.
class EnvEnv
{
public:
    void add (double f, double a)
    {
        if (f <= 0 || a <= 0) return;
        lf_.push_back (std::log2 (f));
        la_.push_back (std::log2 (a));
    }
    bool ready() const { return lf_.size() >= 2; }
    double at (double f) const
    {
        if (lf_.empty()) return 0.0;
        if (f <= 0) return 0.0;
        const double x = std::log2 (f);
        if (x <= lf_.front()) return std::pow (2.0, la_.front());
        if (x >= lf_.back())
        {
            // extrapolar con la pendiente de las dos ultimas armonicas
            const size_t n = lf_.size();
            const double dx = lf_[n - 1] - lf_[n - 2];
            const double dy = la_[n - 1] - la_[n - 2];
            const double s = (dx > 1e-9) ? dy / dx : 0.0;
            return std::pow (2.0, la_.back() + s * (x - lf_.back()));
        }
        const size_t k = std::upper_bound (lf_.begin(), lf_.end(), x) - lf_.begin();
        const size_t k0 = k - 1, k1 = k;
        const double u = (lf_[k1] - lf_[k0]) > 1e-9 ? (x - lf_[k0]) / (lf_[k1] - lf_[k0]) : 0.0;
        return std::pow (2.0, la_[k0] + (la_[k1] - la_[k0]) * u);
    }
private:
    std::vector<double> lf_, la_;
};

} // namespace

bool SpectralEngine::render (const RenderRequest& req, std::vector<float>& out, std::string& err)
{
    out.clear();
    if (! req.original || req.sourceF0.empty() || req.pitchRatio.empty())
    {
        err = "SpectralEngine: faltan datos en la peticion";
        return false;
    }
    const double sr = req.sampleRate;
    const long N = (long) req.original->size();
    const long a = req.sourceStartSample - (long) req.originalStartSample;
    const long b = std::min (N, a + (long) req.numSamples);
    if (b - a < params_.nFft) { err = "SpectralEngine: region demasiado corta"; return false; }

    std::vector<float> seg (req.original->begin() + a, req.original->begin() + b);

    // --- 1) STFT ---
    int nFrames = 0;
    std::vector<std::complex<float>> spec;
    stftComplex (seg, params_.nFft, params_.hop, nFrames, spec);
    if (nFrames <= 0) { err = "SpectralEngine: sin frames"; return false; }
    const int bins = params_.nFft / 2 + 1;
    const double binHz = sr / (double) params_.nFft;
    const int kShape = 2;   // la forma: el pico y 2 bins a cada lado

    // --- 2) la altura: la fuente (del original) y la razon de la correccion ---
    auto f0Fuente = [&] (double frame) -> double {
        return sampleCurve (req.sourceF0, frame, req.totalFrames);
    };
    auto razonEn = [&] (double frame) -> double {
        return sampleCurve (req.pitchRatio, frame, req.totalFrames);
    };

    // --- 3) resintesis frame a frame ---
    //
    // En cada frame:
    //   1) las ARMONICAS de la fuente: su frecuencia (m*F0), su amplitud y su
    //      FORMA (el pico y su leakage), todo centrado en la posicion
    //      fraccional de su frecuencia;
    //   2) el RESIDUO es el espectro menos las armonicas (lo que no es
    //      armonica: el ruido, las consonantes, la respiracion);
    //   3) cada armonica se coloca en m*F0_objetivo, con la MISMA forma
    //      (trasladada) y con la fase ajustada al cambio de frecuencia;
    //   4) el residuo se conserva en su sitio, con su fase.
    //
    // TRES ERRORES QUE HACEIAN QUE ESTE MOTOR NO FUNCIONARA (medido: +100
    // cents pedidos y +0 aplicados, la energia se quedaba en la altura
    // original):
    //   a) al mover la armonica solo se quitaba el 50 % de su magnitud del
    //      sitio viejo (un "delta de 3 bins" que no cubria el pico entero);
    //   b) la posicion nueva se calculaba como "el bin del pico viejo mas el
    //      desplazamiento", pero el pico real no esta en el bin de la
    //      frecuencia nominal (el leakage lo desplaza): la armonica aterrizaba
    //      a 228 Hz cuando se pedian 233;
    //   c) el ajuste de fase usaba el CENTRO del frame (un termino constante)
    //      en vez de su INICIO: la fase ya no progresaba con la frecuencia
    //      nueva y el solape de frames se cancelaba (la armonica movida
    //      desaparecia del resultado: 8 veces menos energia a 233 que a 220).
    //
    // Y una leccion: este motor "no deformaba" (LSD 0,03) y el porton lo
    // APROBO... sin corregir nada. Por eso el porton necesita ademas
    // comprobar que la correccion se aplica (C6).
    std::vector<std::complex<float>> outSpec (spec.size());

    // D-056: la fase de cada armonica se INTEGRA entre frames, no se usa la
    // medida de cada frame. Con la fase medida, cada frame trae un error de
    // fase propio (el pico cae en distinto sitio, arg(row[jm]) cambia) y el
    // solape de frames se CANCELA: medido con +100 c constante en A/1, -3,3 dB
    // de nivel con la fase correcta en frecuencia (100,4 c aplicados). El
    // porton lo nota en C2/C3a/C3b (el nivel entra en la envolvente).
    //   - la fase del CENTRO del lobulo se extrapola del bin del pico con la
    //     rampa de la ventana (Hann simetrica: -pi*(N-1)/N por bin);
    //   - entre frames se acumula la DESVIACION envuelta (la fase avanza lo
    //     que su frecuencia medida + un resto pequeno) y se suma el avance de
    //     la frecuencia NUEVA: la frecuencia cambia, el resto no.
    const int maxArm = (int) (sr * 0.48 / std::max (30.0, params_.fmin)) + 4;
    std::vector<double> faseSin ((size_t) maxArm, 0.0);
    std::vector<double> faseMed ((size_t) maxArm, 0.0);
    std::vector<char>   faseInit ((size_t) maxArm, 0);
    std::vector<int>    faseFrame ((size_t) maxArm, -1);
    const double kDosPi = 2.0 * 3.14159265358979323846;

    for (int f = 0; f < nFrames; ++f)
    {
        const std::complex<float>* row = &spec[(size_t) f * (size_t) bins];
        std::vector<float> magnitudes ((size_t) bins);
        for (int k = 0; k < bins; ++k) magnitudes[(size_t) k] = std::abs (row[k]);

        // la altura fuente y objetivo en el centro del frame
        //
        // I-13 (D-111): el frame de la curva es ABSOLUTO (muestra absoluta del
        // centro del frame). Antes se usaba `a`, que es la posicion DENTRO del
        // bufer (`sourceStartSample - originalStartSample`) y no lleva
        // `originalStartSample`: una peticion que no empieza en la muestra 0
        // leia la curva desplazada (en el adaptador, 8 frames en vez de 1738) y
        // caia en el relleno plano del principio.
        const double frame =
            ((double) req.sourceStartSample + (double) f * params_.hop +
             0.5 * (double) params_.nFft) / req.curveHop;
        const double f0s = f0Fuente (frame);
        const double f0t = f0s * razonEn (frame);

        // El frame se COPIA cuando no hay altura, cuando la fuente no tiene voz
        // (la mascara del contrato manda) o cuando la correccion pedida es
        // practicamente nula (< 0,5 cent).
        //
        // En los huecos sin nota (respiraciones, silencios entre notas) el
        // tracker puede dar un F0 espurio y BAJO, y a f0 bajo los lobulos de
        // armonicos adyacentes se solapan (la forma ocupa kShape = 2 bins a cada
        // lado): el residuo se cuenta dos veces y la energia se dispara. Medido
        // en A/1: estallidos de +11 dB (RMS 0,33 donde el original tiene 0,09)
        // en 29,4 s, 65,4 s y 67,7 s, todos en huecos sin nota, con el pico
        // clampado a 1,0 (1.044 muestras). Copiar el frame donde no se corrige
        // es EXACTO (aqui el motor debe ser transparente) y no pierde nada.
        const int vi = (int) std::llround (frame);
        const bool conVoz = req.sourceVoiced.empty()
                            || (vi >= 0 && vi < (int) req.sourceVoiced.size()
                                && req.sourceVoiced[(size_t) vi] != 0);
        const double centsPedidos = (f0s > 0.0 && f0t > 0.0)
                                      ? 1200.0 * std::log2 (f0t / f0s) : 0.0;
        if (! conVoz || f0s <= 0.0 || f0t <= 0.0
            || std::fabs (centsPedidos) < 0.5)
        {
            for (int k = 0; k < bins; ++k)
                outSpec[(size_t) f * (size_t) bins + (size_t) k] = row[k];
            continue;
        }

        // --- las armonicas de la fuente ---
        struct Harm {
            double f;                       // la frecuencia medida (pico)
            double a;                       // su amplitud (pico parabolico)
            double c0;                      // su centro fraccional (en bins)
            int    jm = 0;                  // el bin del pico
            std::vector<float> forma;       // la forma normalizada
        };
        std::vector<Harm> arms;
        const int mMax = (int) ((sr * 0.48 - 100.0) / f0s);
        const double pasoBins = f0s / binHz;          // bins entre armonicos
        const int radio = std::max (1, (int) std::lround (0.30 * pasoBins));
        for (int m = 1; m <= mMax; ++m)
        {
            const double fh = (double) m * f0s;
            if (fh >= sr * 0.48) break;
            // La posicion del armonico se MIDE en el espectro (pico local),
            // no se toma de m*F0. El F0 del tracker tiene un error que se
            // MULTIPLICA por m (a 3 kHz, un 2 % de error son 8 bins): el
            // modelo restaba en un sitio donde no habia pico, el original se
            // quedaba en el residuo y la armonica movida se sumaba a el.
            // Medido con un shift constante de +100 c en A/1: el motor
            // aplicaba 64 c (mediana) y perdia 3,8 dB de nivel.
            const int jc = std::max (1, std::min (bins - 2,
                                                 (int) std::llround (fh / binHz)));
            int jm = jc;
            for (int j = std::max (1, jc - radio);
                 j <= std::min (bins - 2, jc + radio); ++j)
                if (magnitudes[(size_t) j] > magnitudes[(size_t) jm]) jm = j;
            double c0 = (double) jm;
            double aMag = (double) magnitudes[(size_t) jm];
            // refinamiento parabolico del pico (posicion y amplitud)
            const double y0 = (double) magnitudes[(size_t) jm - 1];
            const double y1 = (double) magnitudes[(size_t) jm];
            const double y2 = (double) magnitudes[(size_t) jm + 1];
            const double den = y0 - 2.0 * y1 + y2;
            if (std::fabs (den) > 1e-12)
            {
                const double d = 0.5 * (y0 - y2) / den;
                if (std::fabs (d) <= 0.5)
                {
                    c0 = (double) jm + d;
                    aMag = y1 - 0.25 * (y0 - y2) * d;
                }
            }
            if (aMag <= 1e-6) continue;
            Harm h;
            h.f = c0 * binHz; h.a = aMag; h.c0 = c0; h.jm = jm;
            // La FORMA se toma de los bins REALES del lobulo (offsets enteros
            // desde el pico). Antes se interpolaba en posiciones fraccionales:
            // al restar, el valor reconstruido no coincidia con el bin y el
            // residuo se quedaba con energia (o se pasaba y se recortaba a 0).
            h.forma.resize ((size_t) (2 * kShape + 1));
            for (int d = -kShape; d <= kShape; ++d)
            {
                const int j = jm + d;
                h.forma[(size_t) (d + kShape)] =
                    (j >= 0 && j < bins) ? (float) ((double) magnitudes[(size_t) j] / aMag)
                                         : 0.f;
            }
            arms.push_back (h);
        }

        // --- el residuo: el espectro sin las armonicas ---
        //
        // Se quitan los bins del lobulo (los MISMOS que se van a recolocar), asi
        // la resta es exacta y la energia se conserva: la armonica movida lleva
        // lo que se quito. Antes se restaba un lobulo interpolado que no
        // coincidia con los bins reales.
        std::vector<float> resid (magnitudes);
        for (const Harm& h : arms)
            for (int d = -kShape; d <= kShape; ++d)
            {
                const int j = h.jm + d;
                if (j >= 0 && j < bins) resid[(size_t) j] = 0.f;
            }

        // --- la envolvente trazada por las armonicas (los formantes) ---
        EnvEnv envH;
        for (const Harm& h : arms) envH.add (h.f, h.a);

        // --- el nuevo espectro (en POTENCIA) ---
        //
        // Se reparte la POTENCIA (magnitud al cuadrado) del lobulo a sus
        // posiciones nuevas con pesos que suman 1: la suma de cuadrados se
        // conserva. Repartir la MAGNITUD (interpolar) parte el pico entre dos
        // bins a medias y la energia cae a la mitad (-3 dB): medido con +100 c
        // constante en A/1, -3,1 dB repartidos por banda (peor cuanto mas bajo
        // el armonico), que es exactamente lo que predice el reparto lineal.
        std::vector<float> newPot ((size_t) bins, 0.f);
        std::vector<float> newPh ((size_t) bins, 0.f);
        std::vector<float> mejor ((size_t) bins, 0.f);
        for (int k = 0; k < bins; ++k) newPh[(size_t) k] = (float) std::arg (row[k]);
        // el residuo es INCOHERENTE con las armonicas: se suma su potencia
        for (int k = 0; k < bins; ++k)
        {
            const float r = resid[(size_t) k];
            if (r > 0.f) newPot[(size_t) k] += r * r;
        }

        // El cociente de envolvente de CADA armonica, quitando la TENDENCIA de
        // la fuente. Aplicarlo entero mueve tambien la PENDIENTE espectral (la
        // glotal), que no es un formante: a +100 c eso quita 0,84 dB/octava y
        // deja el espectro inclinado (-0,10 dB en 0-300 Hz, -1,31 dB en
        // 8-16 kHz, medido en A/1) y rompe C4. Melodyne no lo hace. Se ajusta
        // log2 E = a + b*log2 f por minimos cuadrados y se aplica SOLO la
        // desviacion respecto a esa recta: los formantes se preservan, el
        // balance tonal no se mueve.
        std::vector<double> cociente (arms.size(), 1.0);
        if (P_ENV_PRESERVE_FORMANTS && envH.ready() && arms.size() >= 4)
        {
            // La tendencia es LOCAL (media de log2 A en +-0,5 octavas): una
            // recta global no quita la CURVATURA (el espectro de una voz se
            // empina por encima de 5 kHz) y dejaba -1,46 dB en 8-16 kHz. Lo que
            // se aplica es la DESVIACION respecto a la tendencia local: los
            // picos de los formantes se preservan, la pendiente glotal no se
            // mueve.
            const size_t na = arms.size();
            std::vector<double> lx (na), la (na), desv (na, 0.0);
            for (size_t i = 0; i < na; ++i)
            {
                lx[i] = std::log2 (arms[i].f);
                la[i] = std::log2 (arms[i].a);
            }
            for (size_t i = 0; i < na; ++i)
            {
                double suma = 0.0;
                int cuenta = 0;
                for (size_t j = 0; j < na; ++j)
                    if (std::fabs (lx[j] - lx[i]) <= 0.5)
                    {
                        suma += la[j];
                        ++cuenta;
                    }
                if (cuenta > 0)
                    desv[i] = la[i] - suma / (double) cuenta;
            }
            // la desviacion en una frecuencia cualquiera: interpolada entre los
            // armonicos vecinos (asi el estimador es suave)
            auto desvEn = [&] (double f) -> double
            {
                const double x = std::log2 (f);
                if (x <= lx.front()) return desv.front();
                if (x >= lx.back()) return desv.back();
                size_t k = 1;
                while (k < na && lx[k] < x) ++k;
                const double dx = lx[k] - lx[k - 1];
                const double u = (dx > 1e-9) ? (x - lx[k - 1]) / dx : 0.0;
                return desv[k - 1] + (desv[k] - desv[k - 1]) * u;
            };
            for (size_t i = 0; i < na; ++i)
            {
                const double fNew = arms[i].f * (f0t / f0s);
                const double d = desvEn (fNew) - desv[i];
                cociente[i] = std::min (2.0, std::max (0.5, std::pow (2.0, d)));
            }
        }

        for (size_t iHarm = 0; iHarm < arms.size(); ++iHarm)
        {
            const Harm& h = arms[iHarm];
            const double fNew = h.f * (f0t / f0s);
            if (fNew >= sr * 0.48) break;
            // la amplitud nueva = A_m * E(nuevo)/E(viejo): el cociente de la
            // envolvente (los formantes) no cambia con la altura
            const double amp = h.a * cociente[iHarm];
            // La FORMA esta anclada al bin del pico (d = 0 es el bin jm), pero
            // el PICO real esta en jm + d0 (d0 de la parabola): al colocar la
            // forma hay que corregir d0, o el armonico aterriza d0 bins (hasta
            // medio bin, ~100 cents en el fundamental) fuera.
            const double d0 = h.c0 - (double) h.jm;
            const double cNuevo = fNew / binHz;         // el pico nuevo
            const double c1 = cNuevo - d0;              // el ancla de la forma
            // --- la fase: extrapolada al CENTRO y integrada entre frames ---
            const double rampaVentana = 3.14159265358979323846
                                        * (double) (params_.nFft - 1) / (double) params_.nFft;
            const double phiC = (double) std::arg (row[h.jm])
                                + rampaVentana * ((double) h.jm - h.c0);
            const int mi = (int) std::lround (h.f / std::max (1e-9, f0s));
            double base = phiC;
            if (mi >= 1 && mi < maxArm)
            {
                if (! faseInit[(size_t) mi])
                {
                    faseSin[(size_t) mi] = phiC;
                    faseInit[(size_t) mi] = 1;
                }
                else
                {
                    const int df = std::max (1, f - faseFrame[(size_t) mi]);
                    const double avanceMed = kDosPi * h.f * (double) params_.hop
                                             * (double) df / sr;
                    double desv = phiC - faseMed[(size_t) mi] - avanceMed;
                    desv -= kDosPi * std::round (desv / kDosPi);   // a [-pi, pi]
                    faseSin[(size_t) mi] = faseSin[(size_t) mi]
                                           + kDosPi * fNew * (double) params_.hop
                                             * (double) df / sr
                                           + desv;
                }
                faseMed[(size_t) mi] = phiC;
                faseFrame[(size_t) mi] = f;
                base = faseSin[(size_t) mi];
            }
            for (int d = -kShape; d <= kShape; ++d)
            {
                const double mag = amp * (double) h.forma[(size_t) (d + kShape)];
                if (mag <= 0.0) continue;
                const double pos = c1 + (double) d;     // el bin d, en su sitio nuevo
                const int k0 = (int) std::floor (pos);
                const double u = pos - (double) k0;
                for (int kk = k0; kk <= k0 + 1; ++kk)
                {
                    if (kk < 0 || kk >= bins) continue;
                    const double w = (kk == k0) ? (1.0 - u) : u;
                    if (w <= 0.0) continue;
                    newPot[(size_t) kk] += (float) (mag * mag * w);
                    if (mag > mejor[(size_t) kk])
                    {
                        mejor[(size_t) kk] = (float) mag;
                        // Fase del lobulo nuevo: la del centro integrada menos
                        // la rampa de la ventana hasta el bin (D-056).
                        // Probado ademas el cambio de signo de la transformada
                        // del Hann (pi entre 1 y 2 bins): EMPEORA (0,6 c -1,52,
                        // 100 c -3,30 dB contra -0,12/-1,35). La rampa ya lo
                        // absorbe; el signo se duplicaba.
                        newPh[(size_t) kk] = (float) (base
                                              - rampaVentana * ((double) kk - cNuevo));
                    }
                }
            }
        }

        for (int k = 0; k < bins; ++k)
            outSpec[(size_t) f * (size_t) bins + (size_t) k] =
                std::polar ((float) std::sqrt ((double) newPot[(size_t) k]),
                            newPh[(size_t) k]);
    }

    // --- 4) ISTFT ---
    std::vector<float> y;
    istftComplex (outSpec, params_.nFft, params_.hop, nFrames, y);

    out.swap (y);
    return true;
}

} // namespace pitchcore

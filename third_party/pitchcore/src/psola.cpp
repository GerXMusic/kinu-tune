#include "pitchcore/psola.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace pitchcore {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Longitud del grano en las zonas SIN altura (no hay periodo al que
// sincronizarse). Corta para no smeared las consonantes, larga para que el
// solape sea suave.
constexpr double kUnvoicedGrainSec = 0.006;

// Correlacion minima para dar por bueno un tramo. El tracker puede marcar
// "con voz" un aliento o un sopido (que no son periodicos): si el motor le
// creyera, resintetizaria ruido con granos sincronos y saldia una
// oscilacion (medido: a los 86 s el render oscilaria a 10 kHz donde la voz
// es una pendiente suave). Con este umbral, esos tramos se tratan como
// sin voz (con granos fijos) ✓.
constexpr double kMinCorr = 0.55;

// Normalizacion de la ganancia local: se compara el RMS de la salida con el de
// la fuente en una ventana y se corrige (suavizado, acotado).
void applyLocalGainMatch (std::vector<float>& out, const RenderRequest& req,
                          const std::vector<double>& markPos, int nOut)
{
    if (nOut <= 0 || markPos.empty()) return;
    const double sr = req.sampleRate;
    const long half = (long) (2.0 * req.curveHop);
    constexpr double kMinRms = 1e-6;
    constexpr double kMinGain = 0.25;
    constexpr double kMaxGain = 4.0;
    constexpr int kSmoothing = 2;

    // el RMS de la fuente y el de la salida, en ventanas centradas en las marcas
    std::vector<double> gan ((size_t) nOut, 1.0);
    const int n = (int) markPos.size();
    std::vector<double> g (n, 1.0);
    for (int i = 0; i < n; ++i)
    {
        const long c = (long) markPos[(size_t) i];
        double es = 0, eo = 0;
        long cnt = 0;
        for (long k = -half; k < half; ++k)
        {
            const long s = c + k;
            const long o = s - req.sourceStartSample;
            if (o < 0 || o >= nOut) continue;
            const double v = (double) out[(size_t) o];
            eo += v * v;
            const long os = req.sourceStartSample + o;
            double w = 0.0;
            if (readOriginal (req, os, w))
                es += w * w;
            ++cnt;
        }
        if (cnt < 8) continue;
        const double rmsO = std::sqrt (eo / (double) cnt);
        const double rmsS = std::sqrt (es / (double) cnt);
        if (rmsS < kMinRms || rmsO < kMinRms) continue;
        g[(size_t) i] = std::min (kMaxGain, std::max (kMinGain, rmsS / rmsO));
    }
    // suavizado entre marcas consecutivas
    for (int i = 0; i + 1 < n; ++i)
    {
        for (int k = -kSmoothing; k <= kSmoothing; ++k)
        {
            const int j = i + k;
            if (j < 0 || j >= n) continue;
            g[(size_t) i] += (g[(size_t) j] - g[(size_t) i]) / (double) (2 * kSmoothing + 1);
        }
    }
    // aplicar a la salida, entre las marcas
    for (int i = 0; i + 1 < n; ++i)
    {
        const long a = (long) markPos[(size_t) i];
        const long b = (long) markPos[(size_t) i + 1];
        const double ga = g[(size_t) i], gb = g[(size_t) i + 1];
        for (long s = a; s < b && s < nOut; ++s)
        {
            const double u = (b > a) ? (double) (s - a) / (double) (b - a) : 0.0;
            out[(size_t) s] *= (float) (ga + (gb - ga) * u);
        }
    }
    // Y ANTES de la primera marca y DESPUES de la ultima, la ganancia se
    // extiende (constante): si se dejaba a 1, en el borde de cada nota la
    // ganancia saltaba de 1 a g (un escalon) y el detector de clicks lo
    // marcaba (medido: los clicks de mas que el control eran casi tantos
    // como las notas: 155 de mas para 176 notas).
    {
        const long a0 = (long) markPos[0];
        const long aN = (long) markPos[(size_t) n - 1];
        for (long s = 0; s < a0 && s < nOut; ++s) out[(size_t) s] *= (float) g[0];
        for (long s = aN; s < nOut; ++s) out[(size_t) s] *= (float) g[(size_t) n - 1];
    }
}

// La mascara de voz de la fuente en una posicion (en samples del original).
inline bool vozEn (const RenderRequest& req, long posOriginal)
{
    const double frame = (double) posOriginal / req.curveHop;
    const int i = (int) std::lround (frame);
    if (i < 0 || i >= (int) req.sourceVoiced.size()) return false;
    return req.sourceVoiced[(size_t) i] != 0;
}

} // namespace

std::vector<int> ClassicEngine::detectPitchMarks (const RenderRequest& req)
{
    std::vector<int> marks;
    if (! req.original || req.numSamples <= 0) return marks;

    const long n = req.numSamples;
    const double sr = req.sampleRate;
    const long src0 = req.sourceStartSample;

    // el F0 de la fuente, en samples del original
    auto f0En = [&] (long posOriginal) -> double {
        const double frame = (double) posOriginal / req.curveHop;
        const double v = sampleCurve (req.sourceF0, frame, req.totalFrames);
        return (v > 0) ? v : 0.0;
    };
    auto voicedEn = [&] (long posOriginal) -> bool {
        const double frame = (double) posOriginal / req.curveHop;
        const int i = (int) std::lround (frame);
        if (i < 0 || i >= (int) req.sourceVoiced.size()) return false;
        return req.sourceVoiced[(size_t) i] != 0;
    };

    long pos = 0;
    while (pos < n)
    {
        // el siguiente tramo voiced
        while (pos < n && ! voicedEn (src0 + pos)) ++pos;
        if (pos >= n) break;
        const long runStart = pos;
        while (pos < n && voicedEn (src0 + pos)) ++pos;
        const long runEnd = pos;

        const double f0 = f0En (src0 + runStart);
        if (f0 <= 0) continue;
        const double T = sr / f0;
        if (runEnd - runStart < (long) (T * 2)) continue;   // demasiado corto

        // LA MARCA INICIAL: la muestra mas fuerte del primer periodo, para que
        // el tramo empiece en un pulso glotal y no donde resultara que
        // empezase el flag de voicing (si no, el ataque arranca con la fase
        // equivocada y se oye un escalon).
        // La semilla de cada tramo fija la FASE de todo el tramo (las demas
        // marcas la siguen por correlacion). Antes se tomaba la muestra mas
        // FUERTE en valor absoluto: segun la vocal, el lobulo mas fuerte es el
        // positivo o el negativo, asi que dos tramos seguidos podian quedar en
        // CONTRAPASE (medido: el render a +0,12 donde el original esta a -0,15
        // y un salto de 180 grados al entrar el tramo siguiente). Con el maximo
        // CON SIGNO la fase es la misma en todos los tramos.
        int seed = (int) runStart;
        float best = -1e30f;
        const long seedEnd = std::min (runEnd, runStart + (long) T);
        for (long i = runStart; i < seedEnd; ++i)
        {
            double m = 0.0;
            if (! readOriginal (req, src0 + i, m)) continue;
            if (m > best) { best = (float) m; seed = (int) i; }
        }
        marks.push_back (src0 + seed);

        // caminar un periodo a la vez, dejando que la CORRELACION corrija la
        // prediccion: asi se autocorrige un F0 ligeramente equivocado y no
        // salta de octava cuando un armónico es mas fuerte que el fundamental.
        long current = seed;
        while (true)
        {
            const double f = f0En (src0 + current);
            if (f <= 0) break;
            const double period = sr / f;
            const long currentL = (long) current;
            const long predicted = currentL + (long) std::lround (period);
            if (predicted >= runEnd) break;

            const long radius = std::max (2L, (long) (period * 0.25));
            const long half = std::max (4L, (long) (period * 0.5));
            const long first = std::max (currentL + 2, predicted - radius);
            const long last = std::min (runEnd - 1, predicted + radius);

            int bestP = (int) predicted;
            double bestScore = -2.0;
            for (long cand = first; cand <= last; ++cand)
            {
                double dot = 0, e1 = 0, e2 = 0;
                bool ok = true;
                for (long k = -half; k < half; ++k)
                {
                    const long a = src0 + current + k;
                    const long b = src0 + cand + k;
                    double va = 0.0, vb = 0.0;
                    if (! readOriginal (req, a, va) || ! readOriginal (req, b, vb))
                    { ok = false; break; }
                    dot += va * vb; e1 += va * va; e2 += vb * vb;
                }
                if (! ok) continue;
                const double den = std::sqrt (e1 * e2);
                const double score = (den > 1e-12) ? dot / den : -2.0;
                if (score > bestScore) { bestScore = score; bestP = (int) cand; }
            }
            // la periodicidad: si la correlacion es baja, aqui ya no hay
            // periodo (el tramo se acaba aqui)
            if (bestScore < kMinCorr) break;
            marks.push_back (src0 + bestP);
            current = bestP;
        }
    }
    std::sort (marks.begin(), marks.end());
    marks.erase (std::unique (marks.begin(), marks.end()), marks.end());
    return marks;
}

bool ClassicEngine::render (const RenderRequest& req, std::vector<float>& out, std::string& err)
{
    out.clear();
    if (! req.original || req.numSamples <= 0) { err = "peticion vacia"; return false; }
    const int nOut = req.numSamples;
    const double sr = req.sampleRate;
    const long src0 = req.sourceStartSample;

    // --- los tramos CON voz (en samples locales) ---
    std::vector<std::pair<long, long>> tramos;
    {
        long pos = 0;
        while (pos < nOut)
        {
            while (pos < nOut && ! vozEn (req, src0 + pos)) ++pos;
            if (pos >= nOut) break;
            const long ini = pos;
            while (pos < nOut && vozEn (req, src0 + pos)) ++pos;
            tramos.push_back ({ ini, pos });
        }
    }

    const std::vector<int> marcas = detectPitchMarks (req);
    if (marcas.size() < 2)
    {
        // sin altura que usar: el original tal cual
        out.assign ((size_t) nOut, 0.f);
        for (int i = 0; i < nOut; ++i)
        {
            const long o = src0 + i;
            double v = 0.0;
            if (readOriginal (req, o, v))
                out[(size_t) i] = (float) v;
        }
        return true;
    }

    std::vector<double> syn ((size_t) nOut, 0.0);
    // La suma de las ventanas: sin normalizar, el solape de los granos no suma
    // exactamente 1 (el espaciado de las marcas varia +-1 muestra y el grano
    // se comprime) y queda una modulacion de amplitud.
    std::vector<double> wsum ((size_t) nOut, 0.0);
    std::vector<double> markPos;

    // Traza opcional para diagnosticar clics (paso 2b, D-058): con
    // PITCHCORE_PSOLA_TRACE=<ruta> se escribe la rejilla de granos y la suma
    // de ventanas, para ver donde hay un agujero de cobertura o un salto de
    // periodo sin tener que adivinarlo. Sin la variable no hace nada.
    const char* traza = std::getenv ("PITCHCORE_PSOLA_TRACE");
    std::vector<double> filasTraza;      // 6 por grano
    if (traza != nullptr) filasTraza.reserve (1 << 16);

    // La mascara de periodicidad del MOTOR: la del tracker, pero solo donde
    // hay periodo de verdad (marcas suficientes y con correlacion buena). El
    // resto se trata como zona sin voz.
    std::vector<char> periodico ((size_t) nOut, 0);
    std::vector<std::pair<size_t, size_t>> tramosConMarca;   // [m0, m1] de cada tramo
    for (size_t kt = 0; kt < tramos.size(); ++kt)
    {
        const long ini = tramos[kt].first, fin = tramos[kt].second;
        size_t m0 = 0;
        while (m0 < marcas.size() && marcas[(size_t) m0] < src0 + ini) ++m0;
        if (m0 >= marcas.size() || marcas[(size_t) m0] >= src0 + fin) continue;
        size_t m1 = m0;
        while (m1 + 1 < marcas.size() && marcas[(size_t) m1 + 1] < src0 + fin) ++m1;
        if (m1 < m0 + 1) continue;

        // ¿hay periodo? (densidad de marcas frente a la esperada)
        const double dur = (double) (fin - ini) / sr;
        const int fi = (int) ((src0 + ini) / req.curveHop);
        const double f0 = (fi >= 0 && fi < (int) req.sourceF0.size())
                            ? (double) req.sourceF0[(size_t) fi] : 0.0;
        const double esperado = (f0 > 0) ? dur * f0 : 0.0;
        const double hay = (double) (m1 - m0 + 1);
        if (esperado > 3.0 && hay < 0.5 * esperado) continue;   // no es periodico

        tramosConMarca.push_back ({ m0, m1 });
        // la parte con voz: desde la primera marca hasta la ultima
        const long TaUlt = (m1 > m0) ? (marcas[m1] - marcas[m1 - 1]) : (long) (sr / (f0 > 0 ? f0 : 100.0));
        const long iniV = marcas[m0] - src0;
        const long finV = marcas[m1] - src0 + TaUlt;
        for (long i = std::max (0L, iniV); i <= std::min ((long) nOut - 1, finV); ++i)
            periodico[(size_t) i] = 1;
    }

    // --- los granos de las zonas CON altura (sincronos con el periodo) ---
    //
    // TD-PSOLA de cambio de altura: las marcas de ANALISIS estan en la fuente
    // (una por periodo) y las de SINTESIS se colocan en la salida con el
    // periodo de salida Ts = Ta / razon. Cada grano de salida se toma del
    // analisis mas cercano y su CONTENIDO se comprime (Ts/Ta) para que su
    // periodo sea Ts.
    //
    // OJO: colocar los granos en las marcas de analisis (en vez de recorrer la
    // salida con paso Ts) NO transpone: los granos quedan separados por el
    // periodo viejo y el resultado no cambia de altura (medido: 0,00 cents de
    // correccion pedida, error de mas de 100 cents).
    //
    // Y el recorrido es POR TRAMO con voz: tratar todas las marcas como una
    // sola serie hacia que, al pasar de una nota a otra, se generara un grano
    // gigante (el espaciado entre notas) y quedaran AGUJEROS de silencio en
    // mitad de la frase (medido: el render tenia trozos mudos donde la voz
    // cantaba).
    for (size_t kt = 0; kt < tramosConMarca.size(); ++kt)
    {
        const size_t m0 = tramosConMarca[kt].first;
        const size_t m1 = tramosConMarca[kt].second;
        // el fin del recorrido: la ultima marca mas un periodo
        const long TaUlt = (m1 > m0) ? (marcas[m1] - marcas[m1 - 1]) : 1;
        const double fin = (double) (marcas[m1] - src0) + (double) TaUlt;

        size_t ia = m0;
        double ts = (double) (marcas[m0] - src0);       // posicion de salida
        int guard = 0;
        while (ts < (double) fin && guard++ < 4000000)
        {
            const long tsAbs = src0 + (long) std::llround (ts);
            // el analisis mas cercano a esta posicion de salida (dentro del tramo)
            while (ia + 1 <= m1 &&
                   std::fabs ((double) marcas[ia + 1] - (double) tsAbs) <
                   std::fabs ((double) marcas[ia] - (double) tsAbs))
                ++ia;
            const long ma = marcas[ia];
            // El periodo local. En la ULTIMA marca del tramo no hay "siguiente":
            // antes se tomaba Ta = 1, con lo que L = 1 y NO se emitia grano
            // (hueco de ceros hasta el final del tramo y clic en la union con
            // la nota siguiente: medido, saltos de 0 a 0,108 en una muestra).
            // Se usa el espaciado anterior, que es el periodo local.
            long TaLong = 0;
            if (ia + 1 <= m1) TaLong = marcas[ia + 1] - ma;
            else if (ia > m0) TaLong = ma - marcas[ia - 1];
            else TaLong = 1;
            const double Ta = (double) std::max (2L, TaLong);

            // El ratio EN LA POSICION DE SALIDA (la correccion).
            //
            // El paso de salida sale del ESPACIADO DE ANALISIS (Ta), no del F0
            // de la curva: el F0 del tracker tiene ruido de unas centesimas de
            // tanto y esa JITTER se acumula en el recorrido (desplazando la
            // salida respecto del original: con ratio 1 la correlacion con el
            // original caia a -0,02). Con Ta el ratio 1 es exacto.
            const double frame = ts / req.curveHop;
            const double ratio = sampleCurve (req.pitchRatio, frame, req.totalFrames);
            const double Ts = (ratio > 1e-6) ? (Ta / ratio) : Ta;

            const long centre = (long) std::llround (ts);
            // La ventana se mide en la SALIDA (medio ancho Ts, no Ta): asi dos
            // granos consecutivos (centros a Ts) suman EXACTAMENTE 1 en todo el
            // solape, con cualquier ratio. Antes el grano se esparcia desde la
            // fuente (dest = ts + k*esc): en las BAJADAS (esc < 1) las ventanas
            // no llegaban a la siguiente y wsum caia en la junta; la
            // normalizacion lo compensaba amplificando un borde de ventana
            // (picos retenidos: 0,12 donde el original esta a 0,08) y el salto
            // al entrar el grano siguiente era el clic. Interpolar la fuente
            // (en vez de esparcirla) no deja huecos y conserva el centro
            // fraccional (sin jitter de periodo).
            const double esc = Ts / (double) Ta;
            const long alcance = (long) std::ceil (Ts);
            if (alcance >= 2 && esc > 1e-6)
            {
                for (long j = centre - alcance; j <= centre + alcance; ++j)
                {
                    if (j < 0 || j >= nOut) continue;
                    const double k = (double) j - ts;       // offset de SALIDA
                    if (std::fabs (k) > Ts) continue;
                    const double sp = (double) ma + k / esc;  // posicion en la fuente
                    const long s0 = (long) std::floor (sp);
                    const double fs = sp - (double) s0;
                    const long base = (long) req.originalStartSample;
                    const long last = (long) req.original->size() - 1;
                    const long s0r = s0 - base;
                    if (s0r < 0 || s0r + 1 > last) continue;
                    // Interpolacion CUBICA (Catmull-Rom), no lineal: la lineal
                    // deja un CODO en cada muestra (derivada discontinua), y el
                    // detector de clicks mide la segunda diferencia: ese codo se
                    // cuenta como clic en cada pico (medido: el render con
                    // 0,021 de escalon donde el original tiene 0,0037). La
                    // cubica es C1 y ademas conserva mejor los agudos.
                    auto at = [&] (long idx) {
                        long r = idx - base;
                        if (r < 0) r = 0;
                        if (r > last) r = last;
                        return (double) (*req.original)[(size_t) r];
                    };
                    const double p0 = at (s0 - 1), p1 = at (s0);
                    const double p2 = at (s0 + 1), p3 = at (s0 + 2);
                    const double ca = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
                    const double cb = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
                    const double cc = -0.5 * p0 + 0.5 * p2;
                    const double v = ((ca * fs + cb) * fs + cc) * fs + p1;
                    const double w = 0.5 * (1.0 + std::cos (kPi * k / Ts));
                    syn[(size_t) j] += w * v;
                    wsum[(size_t) j] += w;
                }
            }
            markPos.push_back ((double) centre);
            if (traza != nullptr)
            {
                filasTraza.push_back (ts);
                filasTraza.push_back ((double) centre);
                filasTraza.push_back ((double) ma);
                filasTraza.push_back (Ta);
                filasTraza.push_back (Ts);
                filasTraza.push_back (ratio);
            }
            ts += (Ts > 1.0) ? Ts : 1.0;
        }
    }

    if (traza != nullptr)
    {
        const std::string base (traza);
        if (FILE* f = std::fopen ((base + ".granos.csv").c_str(), "w"))
        {
            std::fprintf (f, "ts,centre,ma,Ta,Ts,ratio\n");
            for (size_t g = 0; g + 5 < filasTraza.size(); g += 6)
                std::fprintf (f, "%.6f,%.0f,%.0f,%.0f,%.4f,%.6f\n",
                              filasTraza[g], filasTraza[g + 1], filasTraza[g + 2],
                              filasTraza[g + 3], filasTraza[g + 4], filasTraza[g + 5]);
            std::fclose (f);
        }
        if (FILE* f = std::fopen ((base + ".marcas.csv").c_str(), "w"))
        {
            std::fprintf (f, "i,pos\n");
            for (size_t m = 0; m < marcas.size(); ++m)
                std::fprintf (f, "%zu,%ld\n", m, marcas[m]);
            std::fclose (f);
        }
        if (FILE* f = std::fopen ((base + ".wsum.bin").c_str(), "wb"))
        {
            std::fwrite (wsum.data(), sizeof (double), wsum.size(), f);
            std::fclose (f);
        }
    }

    // --- las zonas SIN periodicidad: el ORIGINAL tal cual ---
    // No se resintetizan (no hay periodo al que sincronizarse) y no se dejan
    // como un empalme de granos fijos: se devuelven tal cual, que es la
    // señal continua. La union con la sintesis se hace con un CRUCE suave
    // (una rampa coseno de unos 10 ms): un corte seco entre dos señales
    // distintas es un escalon, y el detector de clicks lo cuenta (medido:
    // con el corte seco, 10.005 discontinuidades, 11x el control).
    //
    // Y el motor decide que es "sin periodicidad" por si mismo (la
    // correlacion de las marcas), no por lo que diga el tracker: el tracker
    // marca "con voz" los respiraciones entre notas y ahi no hay periodo
    // (resintetizarlo producia una oscilacion de 10 kHz, medido a los 86 s).
    // El peso: 1 en las zonas con periodicidad, 0 en las demas, con el cruce
    // suave en las transiciones.
    // La rampa del cruce tiene que ser MAS LARGA que el desvanecido de
    // un grano (un periodo, 5-10 ms): si no, el borde de la sintesis (que se
    // apaga) queda a medias del cruce y se oye un escalon (medido: el
    // detector de clicks marcaba el 84 % de los casos grandes en los bordes
    // de nota).
    const long rampa = (long) (0.010 * sr);
    const int kSuav = (int) std::max (1L, rampa / 4);
    auto suavizar = [&] (const std::vector<float>& in) {
        // Dos pasadas de media movil +-kSuav dan una transicion de ~4*kSuav
        // muestras (los 10 ms de rampa) en vez de un escalon.
        //
        // OJO: antes esta funcion hacia `aux.swap (tmp)` al final de cada
        // pasada, lo que dejaba en `tmp` la copia SIN suavizar y devolvia el
        // original: el cruce sintesis/original se quedaba en un escalon de una
        // muestra. Medido en A/5 (paso 2b): peso 1 -> 0 en la muestra 1102707
        // (t=22,973 s) y un salto de -0,038 a +0,085 en la salida, que es el
        // clic que el porton contaba (el 84 % de los clics grandes estaban en
        // los bordes de cada tramo con voz).
        std::vector<float> cur (in);
        for (int pas = 0; pas < 2; ++pas)
        {
            std::vector<float> dst (cur.size(), 0.f);
            for (int i = 0; i < nOut; ++i)
            {
                double acc = 0; int cnt = 0;
                for (int j = std::max (0, i - kSuav); j <= std::min (nOut - 1, i + kSuav); ++j)
                { acc += cur[(size_t) j]; ++cnt; }
                dst[(size_t) i] = (float) (acc / cnt);
            }
            cur.swap (dst);
        }
        return cur;
    };
    std::vector<float> peso ((size_t) nOut, 0.f);
    for (int i = 0; i < nOut; ++i) peso[(size_t) i] = periodico[(size_t) i] ? 1.f : 0.f;
    peso = suavizar (peso);

    out.assign ((size_t) nOut, 0.f);
    // La normalizacion del solape tiene que ser CONTINUA: con un corte seco
    // (w > 0,25 ? 1/w : 1) la ganancia salta de 1 a 4 al cruzar el umbral y
    // ese escalon es un clic en el borde de cada grano (medido: saltos de 0 a
    // 0,109 en una muestra). 1/max(w, suelo) acota la ganancia y va a 0 de
    // forma continua en el borde (allí syn tambien es 0).
    constexpr double kSuelo = 0.25;
    std::vector<double> premix ((size_t) nOut, 0.0);
    for (int i = 0; i < nOut; ++i)
    {
        const double w = std::max ((double) wsum[(size_t) i], kSuelo);
        premix[(size_t) i] = syn[(size_t) i] / w;
        out[(size_t) i] = (float) premix[(size_t) i];
    }

    // mezclar con el original
    for (int i = 0; i < nOut; ++i)
    {
        const long o = src0 + i;
        double origD = 0.0;
        readOriginal (req, o, origD);
        const float orig = (float) origD;
        // El peso de la sintesis lleva DOS factores: la mascara de
        // periodicidad (peso, ya suavizada) y la COBERTURA real de granos
        // (wsum). Donde solo cubre el borde de un grano (wsum < 1) no se
        // normaliza a plena amplitud: se devuelve al original, que es la senal
        // correcta. Sin esto, el ultimo grano de un tramo prolongaba la nota
        // anterior dentro de la siguiente con peso 1 (medido: picos al doble
        // de nivel, 0,12 donde el original esta a 0,065, y el escalon al
        // entrar los granos del tramo nuevo). Tambien cubre los huecos que
        // quedan a cero en los bordes de la sintesis.
        const double w = (double) peso[(size_t) i]
                       * std::min (1.0, (double) wsum[(size_t) i]);
        out[(size_t) i] = out[(size_t) i] * w + orig * (1.f - w);
    }

    if (traza != nullptr)
    {
        const std::string base (traza);
        if (FILE* f = std::fopen ((base + ".mix.bin").c_str(), "wb"))
        {
            std::fwrite (peso.data(), sizeof (float), peso.size(), f);
            std::fwrite (premix.data(), sizeof (double), premix.size(), f);
            std::fclose (f);
        }
    }

    // --- igualar la ganancia local a la de la fuente ---
    applyLocalGainMatch (out, req, markPos, nOut);
    return true;
}

} // namespace pitchcore

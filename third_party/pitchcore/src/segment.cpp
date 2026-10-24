#include "pitchcore/segment.h"
#include <algorithm>
#include <cmath>

namespace pitchcore {

double hzToMidi(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }
double midiToHz(double m)  { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

static double medianOf(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

std::vector<Note> segmentNotes(const std::vector<F0Frame>& fr, const SegmentParams& p)
{
    const int n = (int) fr.size();
    std::vector<double> m(n, 0.0);
    for (int i = 0; i < n; ++i) m[i] = fr[i].voiced ? hzToMidi(fr[i].f0Hz) : 0.0;

    // Limpieza del contorno: (a) blips vozeados muy cortos, (b) frames atípicos respecto a la mediana vecina
    std::vector<F0Frame> fc(fr);                       // copia local con voiced saneado
    {
        int i = 0;
        while (i < n) {
            if (!fc[i].voiced) { ++i; continue; }
            int j = i; while (j < n && fc[j].voiced) ++j;
            if (j - i < p.minVoicedRun) for (int k = i; k < j; ++k) fc[k].voiced = false;
            i = j;
        }
        std::vector<char> bad(n, 0);
        for (int i2 = 0; i2 < n; ++i2) {
            if (!fc[i2].voiced) continue;
            std::vector<double> w;
            for (int k = std::max(0, i2 - p.outlierWinFrames); k <= std::min(n - 1, i2 + p.outlierWinFrames); ++k)
                if (k != i2 && fc[k].voiced) w.push_back(m[k]);
            if (w.size() >= 3 && std::fabs(m[i2] - medianOf(w)) > p.outlierSemitones) bad[i2] = 1;
        }
        for (int i2 = 0; i2 < n; ++i2) if (bad[i2]) fc[i2].voiced = false;
        // los blips que deja el filtro de atípicos también se descartan
        i = 0;
        while (i < n) {
            if (!fc[i].voiced) { ++i; continue; }
            int j = i; while (j < n && fc[j].voiced) ++j;
            if (j - i < p.minVoicedRun) for (int k = i; k < j; ++k) fc[k].voiced = false;
            i = j;
        }
    }
    const std::vector<F0Frame>& frs = fc;

    // Mediana entre frames vozeados vecinos
    std::vector<double> ms(m);
    const int h = p.medianFrames / 2;
    for (int i = 0; i < n; ++i) {
        if (!frs[i].voiced) continue;
        std::vector<double> w;
        for (int k = std::max(0, i - h); k <= std::min(n - 1, i + h); ++k) if (frs[k].voiced) w.push_back(m[k]);
        ms[i] = medianOf(w);
    }

    const double hopSec = n > 1 ? frs[1].timeSec - frs[0].timeSec : 0.003;
    const int gapFrames = std::max(1, (int) std::lround(p.maxGapSec / hopSec));
    const int lookFrames = std::max(1, (int) std::lround(p.vibratoLookaheadSec / hopSec));

    // --- Ayudante: valle de energia local alrededor de un frame ---
    // Devuelve (profundidad del valle en dB, mediana del contexto).
    // La profundidad es (mediana del contexto) - (minimo en la ventana del valle):
    // > 0 significa que hay un valle (un ataque o una caida).
    // Medido en el corpus real: en las fronteras de notas repetidas a la misma
    // altura el valle es de 2,4-4,9 dB de mediana, frente a 0,26-0,40 dB en el
    // interior de las notas.
    auto valleyDepthAt2 = [&](int frame) -> std::pair<double, double> {
        const int w = std::max(1, (int) std::lround(p.mergeValleyWinSec / hopSec));
        const int c = std::max(2, (int) std::lround(p.mergeValleyCtxSec / hopSec));
        const int lo = std::max(0, frame - w), hi = std::min(n - 1, frame + w);
        double mn = 1e9;
        for (int k = lo; k <= hi; ++k) mn = std::min(mn, frs[k].rmsDb);
        const int clo = std::max(0, frame - c), chi = std::min(n - 1, frame + c);
        std::vector<double> ctx;
        ctx.reserve((size_t)(chi - clo + 1));
        for (int k = clo; k <= chi; ++k) ctx.push_back(frs[k].rmsDb);
        if (ctx.empty()) return { 0.0, 0.0 };
        const double med = medianOf(ctx);
        return { med - mn, med };
    };
    auto valleyDepthAt = [&](int frame) -> double {
        return valleyDepthAt2(frame).first;
    };
    auto contextMedianAt = [&](int frame) -> double {
        return valleyDepthAt2(frame).second;
    };

    std::vector<Note> notes;
    std::vector<double> cur;           // midi de los frames de la nota en curso
    std::vector<double> curRms;        // rms de los frames de la nota en curso
    int first = -1, last = -1, pending = 0, gap = 0;

    auto close = [&](int lastFrame) {
        if (first < 0 || cur.empty()) { first = -1; cur.clear(); curRms.clear(); return; }
        Note nt;
        nt.firstFrame = first; nt.lastFrame = lastFrame;
        nt.t0 = frs[first].timeSec; nt.t1 = frs[lastFrame].timeSec + hopSec;
        nt.midi = medianOf(cur);
        nt.midiRounded = (int) std::lround(nt.midi);
        nt.centsOffset = (nt.midi - nt.midiRounded) * 100.0;
        const bool tooShort = (nt.t1 - nt.t0) < p.minNoteSec;
        if (p.debug)
            std::fprintf(stderr, "[seg] nota %.3f-%.3f (%.0f ms) midi %.2f  %s\n",
                         nt.t0, nt.t1, (nt.t1 - nt.t0) * 1000.0, nt.midi,
                         tooShort ? "DESCARTADA (muy corta)" : "ok");
        if (! tooShort) notes.push_back(nt);
        first = -1; cur.clear(); curRms.clear(); pending = 0; gap = 0;
    };

    for (int i = 0; i < n; ++i) {
        if (!frs[i].voiced) {
            if (first >= 0 && ++gap > gapFrames) close(last);
            continue;
        }
        if (first < 0) { first = i; cur.assign(1, ms[i]); curRms.assign(1, frs[i].rmsDb); last = i; pending = 0; gap = 0; continue; }
        gap = 0;

        // --- Detector de ataques por energia (notas repetidas a la misma altura) ---
        // Firma de ataque: valle local + subida posterior clara. En una caida
        // monotona de energia no hay subida, asi que no se parte (antes eso
        // shreddeaba la nota en trozos de 12 ms que el filtro de duracion
        // descartaba, perdiendo la nota entera).
        if (curRms.size() >= 4)
        {
            const int wPre  = std::max(2, (int) std::lround(p.mergeValleyWinSec / hopSec));
            const int wPost = std::max(3, (int) std::lround(p.attackRiseSec / hopSec));
            const int lo = std::max(0, i - wPre), hi = std::min(n - 1, i + wPost);
            const int mid = i;
            double valley = 1e9;
            for (int k = std::max(0, mid - 1); k <= std::min(n - 1, mid + 1); ++k)
                valley = std::min(valley, frs[k].rmsDb);
            std::vector<double> pre, post;
            for (int k = lo; k < mid; ++k) pre.push_back(frs[k].rmsDb);
            for (int k = mid + 1; k <= hi; ++k) post.push_back(frs[k].rmsDb);
            if (pre.size() >= 2 && post.size() >= 2)
            {
                const double preMed  = medianOf(pre);
                const double postMed = medianOf(post);
                const bool   isAttack = (preMed - valley >= p.attackDipDb)
                                    && (postMed - valley >= p.attackRiseDb);
                // No partir una nota que aun no llega a la duracion minima:
                // el trozo inicial se descartaria y perderiamos informacion.
                const bool   longEnough = (frs[i].timeSec - frs[first].timeSec) >= p.minNoteSec;
                if (isAttack && longEnough)
                {
                    if (p.debug)
                        std::fprintf(stderr, "[seg] ataque en t=%.3f (valle %.2f, pre %.2f, post %.2f) -> parte\n",
                                     frs[i].timeSec, valley, preMed, postMed);
                    close(i - 1);
                    first = i; cur.clear(); curRms.clear();
                    cur.push_back(ms[i]); curRms.push_back(frs[i].rmsDb);
                    last = i; pending = 0;
                    continue;
                }
            }
        }

        const double ref = medianOf(cur);
        if (std::fabs(ms[i] - ref) > p.splitSemitones) {
            if (++pending >= p.splitHoldFrames) {
                // Anti-vibrato: si el F0 vuelve al centro de la nota dentro de la
                // ventana de lookahead, la desviacion era vibrato y no un cambio
                // de altura. Solo se miran frames voiced contiguos (el vibrato es
                // una modulacion dentro de un tramo voiced).
                bool returnsToCentre = false;
                for (int k = i + 1; k < n && k <= i + lookFrames; ++k) {
                    if (!frs[k].voiced) break;             // el tramo voiced se acaba
                    if (std::fabs(ms[k] - ref) <= p.splitSemitones) { returnsToCentre = true; break; }
                }
                if (returnsToCentre) {
                    pending = 0;                            // era vibrato: no se parte
                } else {
                    const int newStart = i - pending + 1;
                    close(newStart - 1);
                    first = newStart; cur.clear(); curRms.clear();
                    for (int k = newStart; k <= i; ++k) if (frs[k].voiced) { cur.push_back(ms[k]); curRms.push_back(frs[k].rmsDb); }
                    pending = 0; last = i;
                    continue;
                }
            }
        } else {
            pending = 0;
        }
        cur.push_back(ms[i]); curRms.push_back(frs[i].rmsDb); last = i;
    }
    close(last);

    // ---- Posproceso: absorber fragmentos de transición y unir vecinas equivalentes ----
    auto recompute = [&](Note& nt) {
        std::vector<double> v;
        for (int k = nt.firstFrame; k <= nt.lastFrame; ++k) if (frs[k].voiced) v.push_back(ms[k]);
        nt.midi = medianOf(v);
        nt.midiRounded = (int) std::lround(nt.midi);
        nt.centsOffset = (nt.midi - nt.midiRounded) * 100.0;
        nt.t0 = frs[nt.firstFrame].timeSec;
        nt.t1 = frs[nt.lastFrame].timeSec + hopSec;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t k = 0; k < notes.size() && !changed; ++k) {
            Note& a = notes[k];
            const bool prevOk = k > 0 && a.t0 - notes[k - 1].t1 <= p.fragGapSec;
            const bool nextOk = k + 1 < notes.size() && notes[k + 1].t0 - a.t1 <= p.fragGapSec;
            const double dp = prevOk ? std::fabs(a.midi - notes[k - 1].midi) : 1e9;
            const double dn = nextOk ? std::fabs(a.midi - notes[k + 1].midi) : 1e9;
            // (1) unir vecinas casi iguales y pegadas, PERO solo si no hay un
            //     valle de energia entre ellas: un ataque delata dos notas
            //     repetidas a la misma altura (el bug clasico de fusion).
            if (nextOk && dn < p.mergeSemitones) {
                const int boundary = a.lastFrame;
                const double dip = valleyDepthAt(boundary);
                if (dip <= p.mergeMaxDipDb) {
                    notes[k + 1].firstFrame = a.firstFrame; recompute(notes[k + 1]);
                    notes.erase(notes.begin() + (long) k); changed = true; continue;
                }
            }
            // (2) fragmento corto entre/junto a una vecina cercana en pitch: se absorbe en la más próxima
            //     PERO solo si no hay un ataque (valle de energía) en la frontera: si lo hay,
            //     son dos notas de verdad y el fragmento no se absorbe.
            if ((a.t1 - a.t0) < p.fragSec && std::min(dp, dn) <= p.fragMaxSemitones) {
                const int boundary = (dp <= dn) ? a.lastFrame : a.firstFrame;
                const double dip = valleyDepthAt(boundary);
                if (dip > p.mergeMaxDipDb) continue;   // hay ataque: no se absorbe
                if (dp <= dn) { notes[k - 1].lastFrame = a.lastFrame; recompute(notes[k - 1]); }
                else          { notes[k + 1].firstFrame = a.firstFrame; recompute(notes[k + 1]); }
                notes.erase(notes.begin() + (long) k); changed = true;
            }
        }
    }
    return notes;
}

} // namespace pitchcore

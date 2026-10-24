#include "pitchcore/synth.h"
#include "pitchcore/segment.h"   // midiToHz()
#include "pitchcore/wav.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace pitchcore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kTableSize = 2048;

struct Rng {
    unsigned s;
    explicit Rng (unsigned seed) : s (seed ? seed : 1u) {}
    double next()                        // [0,1)
    {
        s = s * 1664525u + 1013904223u;
        return double (s >> 8) / double (1u << 24);
    }
    double bipolar() { return next() * 2.0 - 1.0; }
};

// Resonador de 2 polos (formante). y[n] = gain * x[n] + b1*y[n-1] + b2*y[n-2]
struct Resonator {
    double a = 0.0, b1 = 0.0, b2 = 0.0;
    double y1 = 0.0, y2 = 0.0;
    double out = 0.0;

    void setup (double hz, double q, double sr)
    {
        const double w = 2.0 * kPi * hz / sr;
        const double r = std::exp (-w / (2.0 * q));
        const double th1 = 2.0 * r * std::cos (w);
        const double th2 = -r * r;
        const double norm = (1.0 - r) * std::sqrt (1.0 - 2.0 * r * std::cos (2.0 * w) + r * r);
        a = (norm > 1e-9) ? (1.0 - r) / norm : 1.0;
        b1 = th1; b2 = th2;
    }
    double process (double x)
    {
        const double y = a * x + b1 * y1 + b2 * y2;
        y2 = y1; y1 = y;
        out = y;
        return y;
    }
};

} // namespace

SynthNote makeNote (double t0, double t1, double midi)
{
    SynthNote n;
    n.t0 = t0; n.t1 = t1; n.midi = midi;
    return n;
}

double synthF0At (const SynthNote& n, const SynthNote* prev, double t, const SynthParams& p)
{
    double cents = 0.0;

    // Slide desde la nota anterior: arranca en (midi + glideFromCents) y llega a midi.
    if (n.glideFromCents != 0.0)
    {
        const double g = n.glideSec > 0.0 ? n.glideSec : std::min (0.12, (n.t1 - n.t0) * 0.3);
        const double u = (g > 1e-9) ? std::max (0.0, std::min (1.0, (t - n.t0) / g)) : 1.0;
        cents += n.glideFromCents * (1.0 - u);
    }

    // Vibrato (con retardo de entrada).
    if (n.vibCents != 0.0 && n.vibHz > 0.0)
    {
        const double vt = t - n.t0 - n.vibDelaySec;
        if (vt > 0.0) cents += n.vibCents * std::sin (2.0 * kPi * n.vibHz * vt);
    }

    // Deriva lineal dentro de la nota.
    if (n.driftCents != 0.0)
    {
        const double u = (n.t1 > n.t0) ? (t - n.t0) / (n.t1 - n.t0) : 0.0;
        cents += n.driftCents * u;
    }

    (void) prev; (void) p;
    return midiToHz (n.midi + cents / 100.0);
}

std::vector<float> renderSynth (const std::vector<SynthNote>& notesIn,
                                const SynthParams& p,
                                SynthTruth& truth)
{
    const double sr = p.sampleRate;
    double tEnd = 0.0;
    for (const auto& n : notesIn) tEnd = std::max (tEnd, n.t1 + 0.2);
    const long N = std::max (1L, (long) std::lround (tEnd * sr));

    // ---- Tabla de ondas glotal (1/k) ----
    std::vector<double> table (kTableSize, 0.0);
    for (int k = 1; k <= p.nHarmonics; ++k)
    {
        const double a = 1.0 / std::pow (double (k), p.harmonicDecay);
        for (int i = 0; i < kTableSize; ++i)
            table[(size_t) i] += a * std::sin (2.0 * kPi * double (k) * double (i) / double (kTableSize));
    }
    double peak = 1e-9;
    for (double v : table) peak = std::max (peak, std::abs (v));
    for (double& v : table) v /= peak;

    // ---- Formantes (banco de resonadores en paralelo) ----
    std::vector<Resonator> res;
    double gsum = 0.0;
    for (const auto& f : p.formants)
    {
        Resonator r; r.setup (f.hz, f.q, sr); res.push_back (r);
        gsum += f.gain;
    }
    if (gsum < 1e-9) gsum = 1.0;

    // ---- Estado por nota ----
    struct Voice {
        const SynthNote* n;
        const SynthNote* prev;
        double phase = 0.0;
        double jitterPhase = 0.0;
        double jitterVal = 0.0;
    };
    std::vector<Voice> voices;
    voices.reserve (notesIn.size());
    for (size_t i = 0; i < notesIn.size(); ++i)
        voices.push_back ({ &notesIn[i], i > 0 ? &notesIn[i - 1] : nullptr, 0.0, 0.0, 0.0 });

    std::vector<float> out ((size_t) N, 0.0f);
    truth.time.assign ((size_t) N, 0.0);
    truth.f0.assign ((size_t) N, 0.0);
    truth.voiced.assign ((size_t) N, 0);
    truth.notes = notesIn;

    Rng rng (p.seed);

    // LCG de jitter: deriva lenta, correlacionada (filtro de un polo).
    for (long i = 0; i < N; ++i)
    {
        const double t = double (i) / sr;

        // --- fuentes de ruido: respiracion y consonante ---
        double noise = 0.0;
        for (size_t vi = 0; vi < voices.size(); ++vi)
        {
            const SynthNote& n = *voices[vi].n;
            // respiracion justo antes del ataque
            if (n.breath && t >= n.t0 - n.breathSec && t < n.t0)
                noise += 0.08 * rng.bipolar();
            // consonante en el ataque (anterior al voicing)
            if (n.consonant && t >= n.t0 && t < n.t0 + n.consonantSec)
                noise += 0.35 * rng.bipolar();
        }
        if (p.noiseLevel > 0.0) noise += p.noiseLevel * rng.bipolar();

        // --- fase glotal de cada nota ---
        double excitation = 0.0;
        for (auto& v : voices)
        {
            const SynthNote& n = *v.n;
            if (t < n.t0 || t >= n.t1) continue;

            double f0 = synthF0At (n, v.prev, t, p);

            // jitter: deriva lenta aleatoria
            if (n.jitterCents != 0.0)
            {
                v.jitterVal = 0.995 * v.jitterVal + 0.005 * rng.bipolar();
                f0 *= std::pow (2.0, (n.jitterCents * v.jitterVal) / 1200.0);
            }

            // envolvente
            double env = 1.0;
            const double rel = std::min (n.releaseSec, (n.t1 - n.t0) * 0.5);
            if (n.attackSec > 0.0 && t < n.t0 + n.attackSec)
                env *= 0.5 * (1.0 - std::cos (kPi * (t - n.t0) / n.attackSec));
            if (rel > 0.0 && t > n.t1 - rel)
                env *= 0.5 * (1.0 + std::cos (kPi * (t - (n.t1 - rel)) / rel));

            // tabla de ondas con interpolacion lineal
            v.phase += f0 / sr;
            while (v.phase >= 1.0) v.phase -= 1.0;
            const double pos = v.phase * double (kTableSize);
            const long i0 = (long) pos;
            const long i1 = (i0 + 1) % kTableSize;
            const double fr = pos - double (i0);
            const double src = table[(size_t) i0] * (1.0 - fr) + table[(size_t) i1] * fr;

            excitation += src * env * n.amp;

            // verdad
            const long idx = i;
            truth.f0[(size_t) idx] = f0;
            truth.voiced[(size_t) idx] = 1;
        }

        // --- filtrado por formantes + ruido ---
        double shaped = 0.0;
        for (size_t k = 0; k < res.size(); ++k)
            shaped += res[k].process (excitation) * p.formants[k].gain / gsum;
        if (res.empty()) shaped = excitation;

        double s = shaped + noise;
        out[(size_t) i] = (float) (s * p.gain);
        truth.time[(size_t) i] = t;
    }

    // Normaliza a pico -1 dBFS para que el nivel no influya en las metricas.
    float pk = 0.0f;
    for (float v : out) pk = std::max (pk, std::abs (v));
    if (pk > 1e-9f)
    {
        const float g = 0.891f / pk;
        for (float& v : out) v *= g;
    }
    return out;
}

// ---------------------------------------------------------------------------
// MIDI minimo (formato 0, una pista)
// ---------------------------------------------------------------------------
namespace {

void putVarLen (std::vector<unsigned char>& b, unsigned v)
{
    unsigned char buf[5];
    int n = 0;
    buf[n++] = (unsigned char) (v & 0x7F);
    while ((v >>= 7) != 0) buf[n++] = (unsigned char) ((v & 0x7F) | 0x80);
    while (n > 0) b.push_back (buf[--n]);
}

void putChunk (std::vector<unsigned char>& b, const char* id, const std::vector<unsigned char>& data)
{
    b.insert (b.end(), id, id + 4);
    const uint32_t len = (uint32_t) data.size();
    // MIDI es big-endian.
    b.push_back ((unsigned char) ((len >> 24) & 0xFF));
    b.push_back ((unsigned char) ((len >> 16) & 0xFF));
    b.push_back ((unsigned char) ((len >> 8) & 0xFF));
    b.push_back ((unsigned char) (len & 0xFF));
    b.insert (b.end(), data.begin(), data.end());
}

} // namespace

bool writeMidi (const std::string& path, const std::vector<SynthNote>& notes, double bpm)
{
    const int div = 480;
    std::vector<unsigned char> track;

    // tempo (meta-event: lleva delta time delante)
    {
        const uint32_t us = (uint32_t) std::lround (60000000.0 / std::max (1.0, bpm));
        putVarLen (track, 0);
        track.push_back (0xFF); track.push_back (0x51); track.push_back (0x03);
        track.push_back ((unsigned char) ((us >> 16) & 0xFF));
        track.push_back ((unsigned char) ((us >> 8) & 0xFF));
        track.push_back ((unsigned char) (us & 0xFF));
    }

    struct Ev { long tick; int order; unsigned char status; unsigned char a, b; };
    std::vector<Ev> evs;
    for (const auto& n : notes)
    {
        const long on  = (long) std::lround (n.t0 * bpm / 60.0 * div);
        const long off = (long) std::lround (n.t1 * bpm / 60.0 * div);
        const int pitch = (int) std::lround (n.midi);
        const int vel = (int) std::lround (90.0 * std::max (0.0, std::min (1.0, n.amp)));
        evs.push_back ({ on,  1, 0x90, (unsigned char) pitch, (unsigned char) vel });
        evs.push_back ({ off, 0, 0x80, (unsigned char) pitch, 0 });
    }
    std::sort (evs.begin(), evs.end(), [] (const Ev& a, const Ev& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order; });

    long last = 0;
    for (const auto& e : evs)
    {
        putVarLen (track, (unsigned) (e.tick - last));
        last = e.tick;
        track.push_back (e.status);
        track.push_back (e.a);
        track.push_back (e.b);
    }
    // fin de pista
    putVarLen (track, 0);
    track.push_back (0xFF); track.push_back (0x2F); track.push_back (0x00);

    std::vector<unsigned char> m;
    const char* hdr = "MThd";
    m.insert (m.end(), hdr, hdr + 4);
    m.push_back (0); m.push_back (0); m.push_back (0); m.push_back (6);   // longitud
    m.push_back (0); m.push_back (0);                                    // formato 0
    m.push_back (0); m.push_back (1);                                    // 1 pista
    m.push_back ((unsigned char) ((div >> 8) & 0xFF));
    m.push_back ((unsigned char) (div & 0xFF));
    putChunk (m, "MTrk", track);

    std::ofstream f (path, std::ios::binary);
    if (! f) return false;
    f.write ((const char*) m.data(), (std::streamsize) m.size());
    return (bool) f;
}

} // namespace pitchcore

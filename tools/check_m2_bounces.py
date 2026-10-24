#!/usr/bin/env python3
"""Veredicto de los bounces 2a y 2c del paso 2 (D-100).

Lee los dos WAV que renderiza el usuario desde el editor y aplica el criterio
escrito en D-100, sin volver a medir nada a mano:

  2a  PSOLA (motor Clasico, build nuevo) contra Renders\\baseline_classic.wav
      -> 0 bytes distintos en el chunk 'data' y 0 muestras distintas.
      El fichero completo puede diferir solo en el chunk 'bext' (marca de
      tiempo que escribe REAPER).

  2c  (a) Pitchcore contra PSOLA  -> |lag| <= 0,05 muestras.
      (b) Pitchcore contra Renders\\Original.wav (el tramo exacto del item,
          D-094) -> +2,52 +- 0,05 muestras con la convencion del arnes
          (lag > 0 = render retrasado; D-094 publico -2,52, misma magnitud).
      (c) control de senal original (D-101): la F0 de la nota --autocorrelacion
          por tramas de 40 ms con salto de 10 ms, mediana en [0,10, 0,55) s,
          referencia la misma ventana de Original.wav-- tiene que quedar a
          +-20 c del objetivo de la edicion y a >=100 c de la fuente. Sin (c)
          no hay PASA: "no desplazar" no puede pasar.

Uso:
  python tools/check_m2_bounces.py
  python tools/check_m2_bounces.py --psola <2a.wav> --pitchcore <2c.wav>
  python tools/check_m2_bounces.py --objetivo-cents -205.05

No renderiza nada: si falta un WAV o no tiene 558.545 muestras, lo dice y no
da veredicto (no se mide lo que no existe).
"""

import argparse
import glob
import os
import re
import struct
import subprocess
import sys
from datetime import datetime

import numpy as np

EXPECTED_SAMPLES = 558_545
LAG_TOLERANCE = 0.05
SOURCE_LAG = 2.52          # convencion del arnes; D-094 lo publico como -2,52
HERE = os.path.dirname(os.path.abspath(__file__))
FORK = os.path.dirname(HERE)

# 2c(c), D-101: mismo estimador que la tabla de D-100.
F0_FRAME_S = 0.040
F0_HOP_S = 0.010
F0_MIN_HZ, F0_MAX_HZ = 70.0, 600.0
F0_WINDOW = (0.10, 0.55)
F0_MEDIAN_CENTS = 20.0     # +-20 c del objetivo de la edicion
F0_MIN_SEP_CENTS = 100.0   # >=100 c de la fuente


def read_wav(path):
    """Devuelve (fmt, chunks, data_bytes, muestras float64)."""
    raw = open(path, "rb").read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("%s no es un WAV RIFF" % path)
    i, chunks, fmt, data = 12, [], None, None
    while i + 8 <= len(raw):
        cid = raw[i:i + 4]
        size = int.from_bytes(raw[i + 4:i + 8], "little")
        body = raw[i + 8:i + 8 + size]
        chunks.append((cid.decode("latin1"), size, i + 8))
        if cid == b"fmt ":
            tag, ch, sr = struct.unpack("<HHI", body[:8])
            bits = struct.unpack("<H", body[14:16])[0]
            fmt = {"tag": tag, "channels": ch, "rate": sr, "bits": bits}
        if cid == b"data":
            data = body
        i += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise ValueError("%s sin fmt o sin data" % path)
    if fmt["bits"] != 24 or fmt["tag"] != 1:
        raise ValueError("%s no es PCM 24 bits" % path)
    a = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)
    v = np.where(v & 0x800000, v - 0x1000000, v)
    return fmt, chunks, data, v.astype(np.float64) / 8388608.0


def run_harness(harness, a, b):
    out = subprocess.run([harness, "--compare", a, b],
                         capture_output=True, text=True).stdout
    m = re.search(r"lag\(A vs B\):\s*(-?[0-9.]+)\s+corr\s+([0-9.]+)\s+rms\s+([0-9.eE+-]+)",
                  out)
    d = re.search(r"muestras distintas:\s*(\d+)\s+de\s+(\d+)", out)
    if not m or not d:
        raise RuntimeError("salida del arnes inesperada:\n" + out)
    return {"lag": float(m.group(1)), "corr": float(m.group(2)),
            "rms": float(m.group(3)),
            "distinct": int(d.group(1)), "common": int(d.group(2)), "raw": out}


def _f0_frame(seg, sr):
    """F0 de una trama por autocorrelacion normalizada; None si no es voiced."""
    n = len(seg)
    seg = seg - seg.mean()
    csum = np.concatenate(([0.0], np.cumsum(seg * seg)))
    total = csum[n]
    if total <= 0.0 or np.sqrt(total / n) < 1e-4:
        return None
    spec = np.fft.rfft(seg, 2 * n)
    ac = np.fft.irfft(np.abs(spec) ** 2)[:n]
    lo = max(1, int(np.floor(sr / F0_MAX_HZ)))
    hi = min(n - 1, int(np.ceil(sr / F0_MIN_HZ)))
    if hi <= lo:
        return None
    ks = np.arange(lo, hi + 1)
    ea = csum[n - ks]
    eb = total - csum[ks]
    den = np.sqrt(np.maximum(ea * eb, 0.0))
    r = np.where(den > 1e-20, ac[ks] / np.maximum(den, 1e-30), 0.0)
    j = int(np.argmax(r))
    if r[j] < 0.5:
        return None
    k = float(ks[j])
    if 0 < j < len(ks) - 1:
        y0, y1, y2 = r[j - 1], r[j], r[j + 1]
        denom = y0 - 2.0 * y1 + y2
        if abs(denom) > 1e-12:
            k += 0.5 * (y0 - y2) / denom
    return sr / k if k > 0 else None


def f0_hz(path, window=F0_WINDOW):
    """F0 mediana (Hz) de una ventana; None si no hay tramas voiced.

    Autocorrelacion por tramas de 40 ms con salto de 10 ms (D-100/D-101).
    """
    fmt, chunks, data, x = read_wav(path)
    sr = fmt["rate"]
    n = int(round(sr * F0_FRAME_S))
    hop = max(1, int(round(sr * F0_HOP_S)))
    a = int(round(window[0] * sr))
    b = int(round(window[1] * sr))
    f0s = []
    for s in range(a, b - n + 1, hop):
        f = _f0_frame(x[s:s + n], sr)
        if f is not None and F0_MIN_HZ <= f <= F0_MAX_HZ:
            f0s.append(f)
    if not f0s:
        return None, 0
    return float(np.median(f0s)), len(f0s)


def cents(a_hz, b_hz):
    return 1200.0 * np.log2(a_hz / b_hz)


def check_format(name, path):
    if not os.path.isfile(path):
        print("  %s: FALTA %s -> sin veredicto" % (name, path))
        return None
    fmt, chunks, data, samples = read_wav(path)
    print("  %s: %d muestras, %d Hz, %d bits, %d canal(es)"
          % (name, len(samples), fmt["rate"], fmt["bits"], fmt["channels"]))
    if (len(samples) != EXPECTED_SAMPLES or fmt["rate"] != 48000
            or fmt["channels"] != 1):
        print("  %s: NO es el material esperado (%d muestras, 48 kHz, mono)"
              % (name, EXPECTED_SAMPLES))
        return None
    return {"fmt": fmt, "chunks": chunks, "data": data, "samples": samples}


def verdict(ok):
    return "PASA" if ok else "FALLA"


def last_synthesis(engine):
    """Ultima marca de tiempo con 'synthesizeRegion: engine=<engine>' en los logs."""
    logs = glob.glob(os.path.join(os.environ.get("APPDATA", ""), "Kinu Tune",
                                  "Logs", "debug_*.log"))
    best = None
    for path in logs:
        try:
            for line in open(path, encoding="utf-8", errors="replace"):
                m = re.match(r"\[(\d{2} \w{3} \d{4} \d{2}:\d{2}:\d{2})\].*"
                             r"synthesizeRegion: engine=(\w+)", line)
                if m and m.group(2) == engine:
                    t = datetime.strptime(m.group(1), "%d %b %Y %H:%M:%S")
                    if best is None or t > best:
                        best = t
        except OSError:
            pass
    return best


def engine_ran(wav_path, engine):
    """I-8: sin una sintesis con ese motor el bounce reproduce el archivo guardado."""
    t = last_synthesis(engine)
    if t is None:
        print("  NO hay ninguna linea 'synthesizeRegion: engine=%s' en los logs: el "
              "render no ejecuto el motor (I-8) -> SIN VEREDICTO" % engine)
        return False
    seconds = (datetime.fromtimestamp(os.path.getmtime(wav_path)) - t).total_seconds()
    if not (0 <= seconds <= 1800):
        print("  la ultima sintesis %s es de %s (%+d s respecto al WAV): no es de este "
              "render -> SIN VEREDICTO" % (engine, t.strftime("%d %H:%M:%S"), int(seconds)))
        return False
    print("  sintesis %s a las %s (%+d s antes del WAV) -> el motor se ejecuto"
          % (engine, t.strftime("%H:%M:%S"), int(seconds)))
    return True


def main():
    ap = argparse.ArgumentParser()
    temp = os.path.join(os.environ.get("TEMP", ""), "opencode", "bounce")
    renders = os.path.join(os.path.dirname(FORK), "Renders")
    ap.add_argument("--psola", default=os.path.join(temp, "2a_psola.wav"))
    ap.add_argument("--pitchcore", default=os.path.join(temp, "2c_pitchcore.wav"))
    ap.add_argument("--baseline", default=os.path.join(renders, "baseline_classic.wav"))
    ap.add_argument("--fuente", default=os.path.join(renders, "Original.wav"))
    ap.add_argument("--arnes", default=os.path.join(
        FORK, "build-dml", "Release", "pitchcore_adapter_check.exe"))
    ap.add_argument("--objetivo-cents", type=float, default=-205.05,
                    help="objetivo de la edicion en cents (midiNote - "
                         "originalMidiNote del JSON del proyecto; D-101)")
    args = ap.parse_args()

    print("== 2a: PSOLA del build nuevo contra %s" % args.baseline)
    psola = check_format("2a", args.psola)
    base = check_format("baseline", args.baseline)
    if psola and base and engine_ran(args.psola, "PSOLA"):
        dbytes = sum(1 for x, y in zip(psola["data"], base["data"]) if x != y)
        print("  chunk 'data': %d B, %d distintos" % (len(psola["data"]), dbytes))
        harness = run_harness(args.arnes, args.psola, args.baseline)
        print("  muestras distintas: %d de %d; lag %.2f; corr %.6f; rms %.3e"
              % (harness["distinct"], harness["common"], harness["lag"],
                 harness["corr"], harness["rms"]))
        bext = next(((off, size) for cid, size, off in psola["chunks"]
                     if cid == "bext"), None)
        whole = open(args.psola, "rb").read()
        ref = open(args.baseline, "rb").read()
        diffs = [i for i in range(min(len(whole), len(ref))) if whole[i] != ref[i]]
        inside = bext is not None and all(bext[0] <= i < bext[0] + bext[1] for i in diffs)
        print("  fichero completo: %d bytes distintos%s"
              % (len(diffs),
                 " (todos dentro de 'bext', marca de tiempo)" if diffs and inside else ""))
        ok = dbytes == 0 and harness["distinct"] == 0 and (not diffs or inside)
        print("  2a: %s%s" % (verdict(ok),
                              "" if ok else "  (criterio: 0 bytes en 'data' y 0 muestras)"))
    else:
        print("  2a: SIN VEREDICTO")

    print("\n== 2c: Pitchcore contra PSOLA y contra la fuente")
    pitch = check_format("2c", args.pitchcore)
    if pitch and psola and engine_ran(args.pitchcore, "Pitchcore"):
        a = run_harness(args.arnes, args.pitchcore, args.psola)
        print("  (a) Pitchcore vs PSOLA: lag %+.2f; corr %.6f; rms %.3e"
              % (a["lag"], a["corr"], a["rms"]))
        ok_a = abs(a["lag"]) <= LAG_TOLERANCE
        print("  2c(a): %s  (criterio |lag| <= %.2f muestras)"
              % (verdict(ok_a), LAG_TOLERANCE))
        if os.path.isfile(args.fuente):
            b = run_harness(args.arnes, args.pitchcore, args.fuente)
            print("  (b) Pitchcore vs fuente (%s): lag %+.2f; corr %.6f; rms %.3e"
                  % (os.path.basename(args.fuente), b["lag"], b["corr"], b["rms"]))
            ok_b = abs(b["lag"] - SOURCE_LAG) <= LAG_TOLERANCE
            print("  2c(b): %s  (criterio %.2f +- %.2f, convencion del arnes; "
                  "D-094 publico el mismo 2,52 con el signo cambiado)"
                  % (verdict(ok_b), SOURCE_LAG, LAG_TOLERANCE))
            ref = run_harness(args.arnes, args.psola, args.fuente)
            print("  referencia: PSOLA del build nuevo vs fuente: lag %+.2f "
                  "(baseline_classic vs fuente: %+.2f)"
                  % (ref["lag"], SOURCE_LAG))
        else:
            print("  2c(b): SIN VEREDICTO (falta %s)" % args.fuente)

        # (c) D-101: control de senal original. Se aplica siempre que se pueda
        # medir; no re-abre ni cierra (a)/(b).
        print("  (c) F0 en %.2f-%.2f s (autocorrelacion 40 ms / salto 10 ms, "
              "mediana)" % F0_WINDOW)
        if not os.path.isfile(args.fuente):
            print("  2c(c): SIN VEREDICTO (falta la fuente %s)" % args.fuente)
        else:
            src_f0, src_n = f0_hz(args.fuente)
            pc_f0, pc_n = f0_hz(args.pitchcore)
            if src_f0 is None or pc_f0 is None:
                print("      fuente %s (%d tramas), 2c %s (%d tramas) -> N/A"
                      % ("%.2f Hz" % src_f0 if src_f0 else "no medida", src_n,
                         "%.2f Hz" % pc_f0 if pc_f0 else "no medida", pc_n))
                print("  2c(c): N/A  (sin tramas voiced: no se cierra 2c)")
            else:
                pc_cents = cents(pc_f0, src_f0)
                dist = abs(pc_cents - args.objetivo_cents)
                sep = abs(pc_cents)
                print("      fuente %7.2f Hz (%d tramas); 2c %7.2f Hz (%d tramas)"
                      " -> %+.1f c" % (src_f0, src_n, pc_f0, pc_n, pc_cents))
                for label, path in (("2a", args.psola), ("baseline", args.baseline)):
                    other, other_n = f0_hz(path)
                    if other is not None:
                        print("      %-8s %7.2f Hz (%3d tramas) -> %+7.1f c"
                              % (label, other, other_n, cents(other, src_f0)))
                print("      objetivo de la edicion %+.2f c; distancia %.1f c; "
                      "separacion de la fuente %.1f c"
                      % (args.objetivo_cents, dist, sep))
                ok_c = dist <= F0_MEDIAN_CENTS and sep >= F0_MIN_SEP_CENTS
                print("  2c(c): %s  (criterio +-%.0f c del objetivo y >=%.0f c de "
                      "la fuente; sin (c) no hay PASA)"
                      % (verdict(ok_c), F0_MEDIAN_CENTS, F0_MIN_SEP_CENTS))
    else:
        print("  2c: SIN VEREDICTO")

    return 0


if __name__ == "__main__":
    sys.exit(main())

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

Uso:
  python tools/check_m2_bounces.py
  python tools/check_m2_bounces.py --psola <2a.wav> --pitchcore <2c.wav>

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
    else:
        print("  2c: SIN VEREDICTO")

    return 0


if __name__ == "__main__":
    sys.exit(main())

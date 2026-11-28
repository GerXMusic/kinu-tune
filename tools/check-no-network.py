#!/usr/bin/env python3
# TuneX - Copyright (C) 2026 Keinoaza
# Part of TuneX, a modified version of PitchNet. Licensed under the GNU
# Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#
# Guard for D-379: TuneX does not talk to the network. The update check that
# asked sessionloops.com about PitchNet releases was removed from the editor,
# and this script fails if a network path comes back.
#
#   python tools/check-no-network.py                     # Source only, exit 0/1
#   python tools/check-no-network.py --binary TuneX.vst3  # also scan the binary
#
# Source is matched case-insensitively. The binary only has to be free of the
# vendor host name: the JUCE WinHTTP imports are reported as a warning, not a
# failure, because the link itself is not changed in this batch.

import argparse
import sys
from pathlib import Path

FORBIDDEN_SOURCE_TOKENS = ("juce::URL", "WebInputStream", "curl", "WinHTTP", "sessionloops")
FORBIDDEN_BINARY_TOKENS = ("sessionloops",)
WARNING_BINARY_TOKENS = ("winhttp",)

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIR = REPO_ROOT / "Source"


def find_source_hits(root: Path):
    hits = []
    if not root.is_dir():
        return hits
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            hits.append((path, 0, "lectura fallida: %s" % error))
            continue
        lowered = text.lower()
        for token in FORBIDDEN_SOURCE_TOKENS:
            needle = token.lower()
            start = 0
            while True:
                index = lowered.find(needle, start)
                if index < 0:
                    break
                line = lowered.count("\n", 0, index) + 1
                hits.append((path, line, text.splitlines()[line - 1].strip()))
                start = index + len(needle)
    return hits


def scan_binary(path: Path):
    data = path.read_bytes().lower()
    failures = []
    warnings = []
    for token in FORBIDDEN_BINARY_TOKENS:
        if token.encode("ascii") in data:
            failures.append(token)
    for token in WARNING_BINARY_TOKENS:
        if token.encode("ascii") in data:
            warnings.append(token)
    return failures, warnings


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Falla si TuneX vuelve a tener una ruta de red (D-379).")
    parser.add_argument("--source", type=Path, default=SOURCE_DIR,
                        help="directorio Source a escanear (por defecto, el del repo)")
    parser.add_argument("--binary", type=Path, default=None,
                        help="binario a escanear, p. ej. TuneX.vst3")
    arguments = parser.parse_args()

    status = 0

    source_hits = find_source_hits(arguments.source)
    print("check-no-network: Source = %s" % arguments.source)
    if source_hits:
        status = 1
        for path, line, text in source_hits:
            print("  FALLO %s:%d: %s" % (path, line, text))
        print("check-no-network: %d coincidencia(s) prohibida(s) en Source" % len(source_hits))
    else:
        print("check-no-network: Source limpio (%d token(s) vigilados)"
              % len(FORBIDDEN_SOURCE_TOKENS))

    if arguments.binary is not None:
        if not arguments.binary.is_file():
            print("check-no-network: FALLO, no existe el binario %s" % arguments.binary)
            return 2
        failures, warnings = scan_binary(arguments.binary)
        print("check-no-network: binario = %s" % arguments.binary)
        if failures:
            status = 1
            for token in failures:
                print("  FALLO el binario contiene '%s'" % token)
        else:
            print("check-no-network: binario sin 'sessionloops'")
        for token in warnings:
            print("  AVISO el binario importa '%s' (JUCE; no se cambia el enlace en esta tanda)"
                  % token)

    if status == 0:
        print("check-no-network: OK")
    return status


if __name__ == "__main__":
    sys.exit(main())

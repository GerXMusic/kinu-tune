#!/bin/bash
# Kinu Tune - Copyright (C) 2026 Keinoaza
# Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
# Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#
# Builds the macOS installer from an unpacked macOS release folder (the one
# that goes into Kinu-Tune-<version>-macOS-arm64.zip), so the installer holds
# exactly the files of the zip:
#
#   packaging/macos/build-pkg.sh <release folder> <version> <output .pkg>
#
# The installer puts everything into the current user's home, at the same
# places as the manual steps in INSTALL-macOS.txt, so it replaces a manual
# install instead of adding a second copy, and needs no administrator
# password:
#   ~/Library/Audio/Plug-Ins/VST3/Kinu Tune.vst3
#   ~/Library/Audio/Plug-Ins/Components/Kinu Tune.component (can be left out)
#   ~/Library/Application Support/Keinoaza/Kinu Tune/models
# Files installed by Installer are not quarantined, so the plug-ins need no
# "Open Anyway" or xattr step; only the unsigned .pkg itself does.
set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <release folder> <version> <output .pkg>" >&2
    exit 1
fi
src=$1
version=$2
out=$3
here=$(cd "$(dirname "$0")" && pwd)

for item in "Kinu Tune.vst3" "Kinu Tune.component" models; do
    [ -e "$src/$item" ] || { echo "missing: $src/$item" >&2; exit 1; }
done

work=$(mktemp -d)
trap 'rm -rf "${work:?}"' EXIT

# One component package per choice. Each root holds only what goes into its
# install location.
component() {
    local name=$1 item=$2 location=$3
    mkdir -p "$work/root-$name"
    ditto --noextattr --noqtn --noacl "$src/$item" "$work/root-$name/$item"
    # Installer "relocates" a bundle to wherever a bundle with the same
    # identifier already is (a build folder, another disk). Turn that off so
    # the plug-in always lands in the plug-in folder.
    pkgbuild --analyze --root "$work/root-$name" "$work/$name.plist" >/dev/null
    local count
    count=$(plutil -extract @ raw -expect array "$work/$name.plist" 2>/dev/null || plutil -convert json -o - "$work/$name.plist" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
    for ((i = 0; i < count; i++)); do
        plutil -replace "$i.BundleIsRelocatable" -bool NO "$work/$name.plist"
    done
    pkgbuild --root "$work/root-$name" \
        --component-plist "$work/$name.plist" \
        --identifier "com.keinoaza.kinutune.$name" \
        --version "$version" \
        --install-location "$location" \
        "$work/built-$name.pkg" >/dev/null
    strip_xattrs "$name"
}

# pkgbuild stores every extended attribute as a "._" entry in the payload,
# and macOS gives each file this script creates a com.apple.provenance
# attribute that cannot be removed. Installer would put those attributes
# (which only mean something on this Mac) on the user's files. Rebuild the
# payload and the bill of materials with cpio and mkbom, which store no
# attributes.
strip_xattrs() {
    local name=$1 expanded="$work/expanded-$name"
    pkgutil --expand "$work/built-$name.pkg" "$expanded"
    (cd "$work/root-$name" && find . | cpio -o --format odc -R 0:0 2>/dev/null) | gzip -9 > "$expanded/Payload"
    # Keep pkgbuild's list (and its ownership) without the "._" entries.
    lsbom "$expanded/Bom" | grep -v '/\._' > "$work/bom-$name.txt"
    rm "$expanded/Bom"
    mkbom -i "$work/bom-$name.txt" "$expanded/Bom"
    local files
    files=$(cd "$work/root-$name" && find . | wc -l | tr -d ' ')
    sed -i '' -E "s/numberOfFiles=\"[0-9]+\"/numberOfFiles=\"$files\"/" "$expanded/PackageInfo"
    pkgutil --flatten "$expanded" "$work/pkgs/$name.pkg"
    if lsbom -s "$expanded/Bom" | grep -q '/\._'; then
        echo "attribute entries left in $name" >&2
        exit 1
    fi
}

mkdir -p "$work/pkgs"
component vst3 "Kinu Tune.vst3" "/Library/Audio/Plug-Ins/VST3"
component au "Kinu Tune.component" "/Library/Audio/Plug-Ins/Components"
component models models "/Library/Application Support/Keinoaza/Kinu Tune"

mkdir -p "$work/resources"
for lang in en ja; do
    mkdir -p "$work/resources/$lang.lproj"
    for f in "$here/installer/$lang.lproj/"*; do
        sed "s/@VERSION@/$version/g" "$f" > "$work/resources/$lang.lproj/$(basename "$f")"
    done
done
sed "s/@VERSION@/$version/g" "$here/installer/distribution.xml" > "$work/distribution.xml"

mkdir -p "$(dirname "$out")"
productbuild --distribution "$work/distribution.xml" \
    --resources "$work/resources" \
    --package-path "$work/pkgs" \
    "$out"
echo "built $out"

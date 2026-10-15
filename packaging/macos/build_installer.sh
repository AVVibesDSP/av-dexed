#!/bin/bash
# Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the macOS installer of AV Dexed: a .pkg that lets the user choose the
# formats (Standalone app, VST3, AU, CLAP), installs each one where macOS looks
# for it (admin password), removes the download quarantine and signs the
# installed bundles locally (ad hoc), plus the .dmg users download: the .pkg,
# two bilingual texts (how to open it, read-me) and an uninstaller.
#
# A new version needs no change here: build the Release bundles, then run
#   packaging/macos/build_installer.sh [bundles folder] [output folder]
# The version comes from the app's Info.plist (CMake project version).
#
#   bundles folder  holds "AV Dexed.app", ".vst3", ".component" and ".clap",
#                   directly or in the CMake layout (Standalone/, VST3/, AU/,
#                   CLAP/). Default: build/source/AVDexed_artefacts/Release.
#   output folder   default: build/macos-installer.
#
# Every bundle must be universal (arm64 + x86_64) and run on macOS 11, as the
# installer promises; the script stops otherwise.
#
# Signing: without settings the bundles are signed ad hoc and the installer
# signs them again on the user's Mac. With a Developer ID:
#   SIGN_APP="Developer ID Application: ..." SIGN_PKG="Developer ID Installer: ..."
# (then notarize; see README.md).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUNDLES="${1:-$ROOT/build/source/AVDexed_artefacts/Release}"
OUT="${2:-$ROOT/build/macos-installer}"
NAME="AV Dexed"
MIN_MACOS="11.0"           # also in distribution.xml.in and the read-me texts
SIGN_APP="${SIGN_APP:--}"  # "-" = ad hoc
SIGN_PKG="${SIGN_PKG:-}"

# Component id | bundle | install location | extra postinstall step | built in
COMPONENTS=(
    "app|$NAME.app|/Applications|lsregister|Standalone"
    "vst3|$NAME.vst3|/Library/Audio/Plug-Ins/VST3||VST3"
    "au|$NAME.component|/Library/Audio/Plug-Ins/Components|aucache|AU"
    "clap|$NAME.clap|/Library/Audio/Plug-Ins/CLAP||CLAP"
)

die() { echo "error: $*" >&2; exit 1; }

quiet() {   # runs a tool; its messages are shown only if it fails
    local log
    log="$("$@" 2>&1)" || { printf '%s\n' "$log" >&2; die "$1 failed"; }
}

version_number() {   # "11.0" -> 110000, to compare versions
    echo "$1" | awk -F. '{ printf "%d\n", $1 * 10000 + $2 * 100 + $3 }'
}

check_binary() {   # bundle: universal, and runs on MIN_MACOS
    local exe archs minos
    exe="$(/usr/libexec/PlistBuddy -c 'Print CFBundleExecutable' "$1/Contents/Info.plist")"
    exe="$1/Contents/MacOS/$exe"
    [ -f "$exe" ] || die "$(basename "$1") has no executable"
    archs=" $(lipo -archs "$exe") "
    case "$archs" in *" arm64 "*) ;; *) die "$(basename "$1") has no arm64 (it has:$archs); build Release universal" ;; esac
    case "$archs" in *" x86_64 "*) ;; *) die "$(basename "$1") has no x86_64 (it has:$archs); build Release universal" ;; esac
    for minos in $(vtool -show-build "$exe" | awk '$1 == "minos" { print $2 }'); do
        [ "$(version_number "$minos")" -le "$(version_number "$MIN_MACOS")" ] ||
            die "$(basename "$1") needs macOS $minos, the installer promises $MIN_MACOS (CMAKE_OSX_DEPLOYMENT_TARGET)"
    done
    [ -n "$(vtool -show-build "$exe" | awk '$1 == "minos"')" ] || die "$(basename "$1"): no minimum macOS found"
}

find_bundle() {   # bundle name, CMake subfolder
    if [ -d "$BUNDLES/$1" ]; then echo "$BUNDLES/$1"
    elif [ -d "$BUNDLES/$2/$1" ]; then echo "$BUNDLES/$2/$1"
    else die "\"$1\" not found in $BUNDLES (or $BUNDLES/$2)"; fi
}

APP="$(find_bundle "$NAME.app" Standalone)"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist")"
[ -n "$VERSION" ] || die "no version in $APP"
echo "AV Dexed $VERSION from $BUNDLES"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/avdexed-installer.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/packages" "$WORK/resources" "$OUT"

for entry in "${COMPONENTS[@]}"; do
    IFS='|' read -r id bundle location extra folder <<< "$entry"
    source_bundle="$(find_bundle "$bundle" "$folder")"
    bundle_version="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$source_bundle/Contents/Info.plist")"
    [ "$bundle_version" = "$VERSION" ] || die "$bundle is $bundle_version, the app is $VERSION"
    check_binary "$source_bundle"

    # Payload: one bundle, signed. Ad hoc, it is complete even before the
    # postinstall step signs it again on the user's Mac.
    root="$WORK/root-$id"
    mkdir -p "$root"
    ditto "$source_bundle" "$root/$bundle"
    xattr -cr "$root/$bundle"
    if [ "$SIGN_APP" = "-" ]; then
        quiet codesign --force --deep --sign - "$root/$bundle"
    else
        quiet codesign --force --deep --options runtime --timestamp --sign "$SIGN_APP" "$root/$bundle"
    fi

    # Never relocated to another copy of the bundle found elsewhere on the disk,
    # and always installed, whatever version is there.
    quiet pkgbuild --analyze --root "$root" "$WORK/$id.plist"
    plutil -replace 0.BundleIsRelocatable -bool NO "$WORK/$id.plist"
    plutil -replace 0.BundleIsVersionChecked -bool NO "$WORK/$id.plist"
    plutil -replace 0.BundleOverwriteAction -string upgrade "$WORK/$id.plist"

    scripts="$WORK/scripts-$id"
    mkdir -p "$scripts"
    case "$extra" in
        lsregister) step='/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$BUNDLE" >/dev/null 2>&1 || true' ;;
        aucache) step='/usr/bin/killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true   # hosts rescan Audio Units' ;;
        *) step=':' ;;
    esac
    if [ "$SIGN_APP" = "-" ]; then
        signstep='/usr/bin/xattr -dr com.apple.quarantine "$BUNDLE" >/dev/null 2>&1 || true
/usr/bin/codesign --force --deep --sign - "$BUNDLE" >/dev/null 2>&1 || true'
    else
        signstep=':   # signed with a Developer ID and notarized: nothing to do'
    fi
    for phase in preinstall postinstall; do
        text="$(cat "$HERE/scripts/$phase.in")"
        text="${text//@BUNDLE@/$bundle}"
        text="${text//@SIGNSTEP@/$signstep}"
        text="${text//@EXTRA@/$step}"
        printf '%s\n' "$text" > "$scripts/$phase"
        chmod 755 "$scripts/$phase"
    done

    quiet pkgbuild --root "$root" --component-plist "$WORK/$id.plist" --scripts "$scripts" \
                   --identifier "org.avvibes.avdexed.pkg.$id" --version "$VERSION" \
                   --install-location "$location" --ownership recommended \
                   "$WORK/packages/AVDexed-$id.pkg"
    echo "  $bundle -> $location"
done

# Installer pages: welcome, read-me, conclusion (English, then Spanish) and the
# licence. Every language folder gets the same pages, so they start in English
# on any Mac (the window's own buttons and steps follow the system language).
# Only the licence changes: its language menu offers English and Español
# (Spanish summary, then the official text).
for lang in en es; do
    lproj="$WORK/resources/$lang.lproj"
    mkdir -p "$lproj"
    for page in "$HERE/resources/pages/"*; do
        sed "s|@VERSION@|$VERSION|g" "$page" > "$lproj/$(basename "$page")"
    done
    case "$lang" in
        en) cp "$ROOT/LICENSE" "$lproj/License.txt" ;;
        es) cat "$HERE/resources/license-es.txt" "$ROOT/LICENSE" > "$lproj/License.txt" ;;
    esac
done
# The AVD logo under the steps, 1x and 2x in one TIFF: a dark D on the light
# window, the original light D in dark mode. Sizes in points: a 182 x 80 corner,
# the logo 130 wide.
command -v swift >/dev/null || die "swift not found (Xcode command line tools)"
for variant in light dark; do
    case "$variant" in
        light) svg="$ROOT/assets/branding/AVDexed_Mark_light.svg"; tiff="background.tiff" ;;
        dark)  svg="$ROOT/assets/branding/AVDexed_Mark.svg";       tiff="background-dark.tiff" ;;
    esac
    for scale in 1 2; do
        quiet swift "$HERE/render_logo.swift" "$svg" "$WORK/logo-$variant-$scale.png" 182 80 130 26 18 "$scale"
    done
    quiet tiffutil -cathidpicheck "$WORK/logo-$variant-1.png" "$WORK/logo-$variant-2.png" -out "$WORK/resources/$tiff"
done

sed "s|@VERSION@|$VERSION|g" "$HERE/distribution.xml.in" > "$WORK/distribution.xml"
PKG="$OUT/$NAME $VERSION.pkg"
rm -f "$PKG"
quiet productbuild --distribution "$WORK/distribution.xml" --resources "$WORK/resources" \
                   --package-path "$WORK/packages" ${SIGN_PKG:+--sign "$SIGN_PKG"} "$PKG"
echo "Installer: $PKG"

# Disk image: what a user downloads (the .pkg alone is not published). The
# numbers keep this order in Finder; the texts use these names.
DMG="$OUT/$NAME $VERSION macOS.dmg"
staging="$WORK/dmg"
mkdir -p "$staging"
sed "s|@VERSION@|$VERSION|g" "$HERE/dmg/how-to-open.txt" > "$staging/1 - HOW TO OPEN - CÓMO ABRIR.txt"
cp "$PKG" "$staging/2 - $NAME $VERSION.pkg"
sed "s|@VERSION@|$VERSION|g" "$HERE/dmg/read-me.txt" > "$staging/3 - READ ME - LÉEME.txt"
cp "$HERE/dmg/uninstall.command" "$staging/4 - Uninstall - Desinstalar.command"
chmod 755 "$staging/4 - Uninstall - Desinstalar.command"
rm -f "$DMG"
quiet hdiutil create -volname "$NAME $VERSION" -srcfolder "$staging" -fs HFS+ -format UDZO -ov "$DMG"
echo "Disk image: $DMG"

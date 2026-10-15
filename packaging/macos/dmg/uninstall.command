#!/bin/sh
# Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Removes what the AV Dexed installer put on this Mac: the app and the VST3,
# AU and CLAP plug-ins. Sounds and settings
# (~/Library/Application Support/AV Vibes/AV Dexed) are kept.

# Installer package id | installed bundle
ITEMS="app|/Applications/AV Dexed.app
vst3|/Library/Audio/Plug-Ins/VST3/AV Dexed.vst3
au|/Library/Audio/Plug-Ins/Components/AV Dexed.component
clap|/Library/Audio/Plug-Ins/CLAP/AV Dexed.clap"

installed() {   # the bundles still on this Mac, one per line
    echo "$ITEMS" | while IFS='|' read -r id item; do
        if [ -e "$item" ]; then echo "$item"; fi
    done
}

echo ""
echo "AV Dexed — Uninstall / Desinstalar"
echo "----------------------------------"
found="$(installed)"
if [ -z "$found" ]; then
    echo "AV Dexed is not installed / AV Dexed no está instalado."
    echo ""
    exit 0
fi
echo "These will be removed / Se quitan:"
echo "$found" | sed 's/^/  · /'
echo ""
echo "Your sounds and settings stay / Tus sonidos y ajustes se quedan:"
echo "  ~/Library/Application Support/AV Vibes/AV Dexed"
echo ""
printf "Continue? / ¿Continuar? [y/n] (s/n): "
read -r answer
answer="$(printf '%s' "$answer" | tr -d ' ' | tr 'A-Z' 'a-z')"
case "$answer" in
    y|yes|s|si|sí|sÍ) ;;
    *) echo "Nothing was removed / Nada se quitó."; echo ""; exit 0 ;;
esac

# Everything runs as root after one administrator prompt (a standard account
# can type an administrator's name and password there). A package receipt is
# forgotten only once its bundle is really gone.
root_script="$(echo "$ITEMS" | while IFS='|' read -r id item; do
    printf "if [ -e '%s' ]; then /bin/rm -rf '%s'; fi; " "$item" "$item"
    printf "[ -e '%s' ] || /usr/sbin/pkgutil --forget org.avvibes.avdexed.pkg.%s >/dev/null 2>&1; " "$item" "$id"
done)/usr/bin/killall -9 AudioComponentRegistrar >/dev/null 2>&1; exit 0"

echo "macOS asks for an administrator password / macOS pide la contraseña de un administrador."
if ! /usr/bin/osascript -e 'on run argv' \
                        -e 'do shell script (item 1 of argv) with administrator privileges' \
                        -e 'end run' "$root_script" >/dev/null 2>&1; then
    echo ""
    echo "Nothing was removed: cancelled, or not an administrator password."
    echo "Nada se quitó: se canceló o la contraseña no es de un administrador."
    echo ""
    exit 1
fi

left="$(installed)"
echo ""
if [ -n "$left" ]; then
    echo "These could not be removed / Esto no se pudo quitar:"
    echo "$left" | sed 's/^/  · /'
    echo ""
    exit 1
fi
echo "Done. AV Dexed was removed / Listo. AV Dexed se quitó."
echo ""

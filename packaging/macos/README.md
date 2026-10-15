# macOS installer

`build_installer.sh` turns the Release bundles into the file users download:

- `AV Dexed <version> macOS.dmg` is the only file to publish. It holds, in this order:
  `1 - HOW TO OPEN - CÓMO ABRIR.txt` (the short Gatekeeper steps, for those who read nothing else),
  `2 - AV Dexed <version>.pkg`, `3 - READ ME - LÉEME.txt` and `4 - Uninstall - Desinstalar.command`. The texts
  use these names. Sources: `dmg/how-to-open.txt`, `dmg/read-me.txt`, `dmg/uninstall.command`.
- `AV Dexed <version>.pkg` (also left next to the `.dmg`) lets the user choose the formats (Standalone app,
  VST3, AU, CLAP) and installs each one where macOS looks for it (administrator password). Then it removes the
  download quarantine and signs the installed bundles locally (ad hoc), because AV Dexed is not notarized yet.
  It has welcome, read-me, licence and conclusion pages, English first with Spanish below. `en.lproj` and
  `es.lproj` get the same pages (`resources/pages`), so the pages start in English on every Mac. The window
  title, step names and buttons come from macOS and follow the system language; a package cannot change them
  or add a language step. Only the licence differs: its language menu offers English (the GPL) and Español
  (`resources/license-es.txt`, an unofficial summary, then the official English text), and a Spanish Mac
  opens it in Español. The AVD logo
  (`assets/branding/AVDexed_Mark.svg`, and `AVDexed_Mark_light.svg` with a dark D for the light window) sits
  under the steps; `render_logo.swift` draws it.

## A new version

1. Set the version in the root `CMakeLists.txt` (`project(AVDexed VERSION x.y.z)`).
2. Build the four formats in Release. Release is universal (arm64 + x86_64) and runs on macOS 11 or later by
   default, also in an older build folder:

   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --target AVDexed_Standalone AVDexed_VST3 AVDexed_AU AVDexed_CLAP -j4
   ```

3. Make the installer (the version is read from the app):

   ```sh
   packaging/macos/build_installer.sh build/source/AVDexed_artefacts/Release build/macos-installer
   ```

   It stops if a bundle has another version, is not universal, or needs a macOS newer than 11.0.

Nothing in `packaging/macos` changes between versions. The texts are in `dmg/` and `resources/`
(`@VERSION@` is filled in). The installer draws the pages through the system's HTML import, not a browser:
keep them to plain paragraphs, lists and headings (boxes and backgrounds come out as bands behind the
lines, and in dark mode it shows all text in white).

## Once there is a Developer ID

Not tried yet (there is no certificate). The script takes the two identities:

```sh
SIGN_APP="Developer ID Application: …" SIGN_PKG="Developer ID Installer: …" \
    packaging/macos/build_installer.sh build/source/AVDexed_artefacts/Release build/macos-installer
```

With `SIGN_APP` the bundles are signed with the hardened runtime and a timestamp, and the installer no longer
removes the quarantine or signs on the user's Mac. If the standalone app needs the microphone under the
hardened runtime, add `--entitlements` with `com.apple.security.device.audio-input` to its `codesign` line.
Then notarize the `.dmg`, which covers the `.pkg` inside it, and staple it (`xcrun notarytool submit "….dmg"
--wait`, `xcrun stapler staple "….dmg"`). Rewrite the "not signed" parts of `dmg/read-me.txt`,
drop `dmg/how-to-open.txt`, and update `resources/pages/Welcome.html` and `ReadMe.html`.

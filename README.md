# AV Dexed

AV Dexed is a free, unofficial fork of [Dexed](https://github.com/asb2m10/dexed), the DX7-style FM synthesizer by
Pascal Gauthier (Digital Suburban). It keeps Dexed's FM engine and all of its classic editing, and adds a simpler
interface for people who just want to play and sound good.

**Version 0.1.0 — public preview.** Source code only for now; no binaries are published yet.

> AV Dexed is a modified version of Dexed 1.1.0 (asb2m10/dexed, base commit `eebb748`), modified by AV Vibes since
> October 2026; the git history shows every change. It is not affiliated with or endorsed by Pascal Gauthier /
> Digital Suburban. Please report AV Dexed problems in this repository, not to the original project.

## What is different

- **Play view:** a sound browser with search, favorites and folders; the FM algorithm in the center, where each
  operator lights up while it sounds and opens in Advanced with a click; Volume, Pan and Cutoff, plus Reverb, Delay and
  Chorus.
- **Advanced view:** every classic Dexed parameter, redrawn.
- **Built-in effects:** reverb, tempo-synced delay and chorus, saved with each sound.
- **Settings:** audio output, instrument options, MIDI inputs, MIDI CC assignments (with invert), SysEx, and About.
- **English and Spanish** interface.
- **Its own plugin identifiers and data folder**, so it can be installed next to the original Dexed.

## User data and compatibility

On macOS, AV Dexed keeps its data in `~/Library/Application Support/AV Vibes/AV Dexed` (`Dexed.xml`, `Cartridges`,
`User Presets` and `Profiles`). On first launch it copies missing settings and banks from the original Dexed folder
(`~/Library/Application Support/DigitalSuburban/Dexed`) without deleting or overwriting anything.

Existing banks and presets keep working. Native `.dexedpreset` files store the sound, the effects and its metadata;
`.syx` keeps the original DX7 bank/voice format, which has no room for the new effects. Because AV Dexed has its own
plugin identifiers, it does not replace an original Dexed instance in an existing DAW project.

## Build

Requirements: CMake 3.22 or newer and a C++17 compiler (on macOS, Xcode or the Command Line Tools).

```sh
git clone --recursive https://github.com/AVVibesDSP/av-dexed.git
cd av-dexed
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Targets: Standalone, VST3 and CLAP, plus AU on macOS. Results are in `build/source/AVDexed_artefacts/`. Plugins are not
copied into your plugin folders unless you add `-DDEXED_COPY_PLUGIN_AFTER_BUILD=ON`. The `libs/JUCE` submodule is
JUCE 8.0.14; to use another JUCE 8 checkout, pass `-DDEXED_JUCE_PATH=/path/to/JUCE`. Add `-DDEXED_BUILD_TESTS=ON` to
build the regression tests. AV Dexed is developed and tested on macOS; Linux and Windows builds are untested.
`Dexed.jucer` is not maintained — use CMake.

## License

AV Dexed is free software under the GNU General Public License, version 3 or later ([LICENSE](LICENSE)), and comes
**without any warranty**. The `source/msfa` engine stays under the Apache License 2.0, JUCE 8 is used under its AGPLv3
open-source option, and the embedded Montserrat fonts use the SIL Open Font License 1.1 (license texts:
[LICENSE](LICENSE), [source/msfa/LICENSE](source/msfa/LICENSE), [assets/fonts/OFL.txt](assets/fonts/OFL.txt)). See
[third-party notices](doc/THIRD_PARTY_NOTICES.md) for every component.

Yamaha and DX7 are trademarks of Yamaha Corporation. AV Dexed is not affiliated with Yamaha.

## Credits & thanks

AV Dexed exists thanks to Dexed and the people who made it. Original Dexed credits:

* DX Synth engine : Raph Levien and the [msfa](https://github.com/google/music-synthesizer-for-android) team
* [Surge Synth Team](https://surge-synth-team.org/) for substantial contributions like microtuning and MPE support.
* Graphical design : AZur Studio
* [Sentinel77](https://github.com/Sentinel77) for numerous engine fixes
* LP Filter : Filatov Vadim (2DaT); taken from the excellent [Obxd](https://obxd.wordpress.com) project
* PPPlay : Great [OPL3](https://github.com/stohrendorf/ppplay) implementation, with documented code :D
* DX7 program compilation : Jean-Marc Desprez (author of [SynprezFM](http://www.synprez.com/SynprezFM))
* DX7 programs : Dave Benson, Frank Carvalho, Tim Conrardy, Jack Deckard, Chris Dodunski, Tim Garrett, Hitaye, Stephan Ibsen, Christian Jezreel, Narfman, Godric Wilkie
* falkTX [distrho](http://distrho.sourceforge.net/)

Also: VU meter by Fulop Nandor; thanks to tico-tico and jeremybernstein. AV Dexed interface and effects: AV Vibes.

---

# AV Dexed — Español

AV Dexed es un fork **gratuito y no oficial** de [Dexed](https://github.com/asb2m10/dexed), el sintetizador FM estilo
DX7 de Pascal Gauthier (Digital Suburban). Conserva el motor FM de Dexed y toda su edición clásica, y agrega una
interfaz más sencilla para quien solo quiere tocar y que suene bien.

**Versión 0.1.0, vista previa pública.** Por ahora solo se publica el código; todavía no hay instaladores.

AV Dexed es una versión modificada de Dexed 1.1.0 (commit base `eebb748`), modificada por AV Vibes desde octubre de
2026; el historial de git muestra cada cambio. No está afiliado ni cuenta con el respaldo de Pascal Gauthier / Digital
Suburban. Reporta los problemas de AV Dexed en este repositorio, no en el proyecto original.

- **Play:** buscador de sonidos con favoritos y carpetas, el algoritmo FM al centro (cada operador brilla mientras
  suena y se abre en Advanced con un clic), Volumen, Paneo, Corte, Reverb, Delay y Chorus.
- **Advanced:** todos los parámetros clásicos de Dexed.
- **Ajustes:** salida de audio, instrumento, entradas MIDI, asignaciones de CC (con invertir), SysEx y Acerca de.
- **Español e inglés.** Identificadores y carpeta de datos propios: se puede instalar junto al Dexed original.

Para compilar, sigue los pasos de **Build** de arriba. Licencia: GNU GPL versión 3 o posterior, **sin ninguna
garantía**; consulta [LICENSE](LICENSE) y los [avisos de terceros](doc/THIRD_PARTY_NOTICES.md). Yamaha y DX7 son marcas
registradas de Yamaha Corporation; AV Dexed no está afiliado con Yamaha.

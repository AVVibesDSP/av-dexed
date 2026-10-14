# Third-party notices

AV Dexed is an unofficial, modified version of Dexed. Listing a project here does not imply its endorsement.

## Dexed

Dexed © 2013-2025 Pascal Gauthier (Digital Suburban) and contributors, licensed under the GNU GPL version 3 or later.
AV Dexed is based on Dexed 1.1.0 (https://github.com/asb2m10/dexed, commit `eebb748`) and has been modified by
AV Vibes since October 2026. Copyright and license notices are kept in the source files; upstream files that AV Vibes
changed say so in their header or appear in the git history.

## Components

| Component | Where | License |
| --- | --- | --- |
| Dexed | everything not listed below (upstream files keep their own notices) | GPL-3.0-or-later |
| msfa (music-synthesizer-for-android), © 2012 Google Inc. and contributors | `source/msfa` | Apache-2.0 (full text in `source/msfa/LICENSE`) |
| JUCE 8.0.14 | `libs/JUCE` (submodule) | AGPLv3 open-source option (combined under GPLv3 section 13) or commercial; JUCE's own bundled SDKs are listed in `libs/JUCE/LICENSE.md` |
| MTS-ESP client, © 2021 ODDSound Ltd. | `libs/MTS-ESP` (submodule) | permissive (see its `LICENSE`) |
| tuning-library, © Paul Walker | `libs/tuning-library` (submodule) | MIT |
| surgesynthteam_tuningui, © Paul Walker | `libs/surgesynthteam_tuningui` (submodule) | MIT |
| clap-juce-extensions, © Paul Walker (bundles the CLAP SDK, MIT) | `libs/clap-juce-extensions` (submodule) | MIT |
| LP filter from the OB-Xd project by Filatov Vadim (2DaT) | `source/PluginFx.cpp`, as in upstream Dexed | see the source header |
| Mark I engine based on PPPlay and its OPL3 implementation | `source/engine/EngineMkI.cpp`, as in upstream Dexed | see the source header |
| DX7 envelope tables from legasynth-0.4.1 | `source/ui/component/DXComponents.cpp`, as in upstream Dexed | see the source header |
| DX7 programs compiled by Jean-Marc Desprez (SynprezFM) and the programmers credited in `README.md` | `assets/builtin_pgm.zip`, as in upstream Dexed | as distributed with Dexed |
| VU meter, originally by Fulop Nandor | `source/ui/component/VUMeter.*` (rewritten) | GPL-3.0-or-later |
| Montserrat fonts, © 2011-2024 The Montserrat Project Authors | `assets/fonts` | SIL Open Font License 1.1 (`assets/fonts/OFL.txt`) |
| Noto Sans fonts, © 2012 Google Inc. (inherited from Dexed, not used by AV Dexed) | `assets/ui/NotoSans-*.ttf` | Apache-2.0 |
| Yamaha TX802 SysEx documentation (inherited from Dexed) | `doc/TX802-SYSEX.pdf` | Yamaha documentation, redistributed as in upstream; not covered by the GPL |
| DX7 SysEx format notes by Ewan A. Macpherson (inherited from Dexed) | `doc/sysex-format.txt` | third-party note, redistributed as in upstream |
| Faust libraries used to generate the delay | `source/effects/AVDexedDelayFaust.h` | LGPL with exception (allows any license for generated code) |

## AV Vibes work

The AV Dexed interface, the reverb, delay and chorus DSP, the icons and the logo are © 2026 AV Vibes. Parts of them are
adapted from other AV Vibes software and are released here by their copyright holder under GPL-3.0-or-later. The
Faust delay (`source/effects/faust/AVDexedDelay.dsp` and the generated `source/effects/AVDexedDelayFaust.h`) is licensed
under the MIT License instead:

> Copyright (c) AV Vibes. Permission is hereby granted, free of charge, to any person obtaining a copy of this software
> and associated documentation files (the "Software"), to deal in the Software without restriction, including without
> limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
> Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions: The
> above copyright notice and this permission notice shall be included in all copies or substantial portions of the
> Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
> LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
> THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
> CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
> THE SOFTWARE.

The reverb is built on JUCE's `juce::dsp::Reverb` (based on the FreeVerb technique and tunings by Jezar at Dreampoint, public domain).

## Trademarks

Yamaha and DX7 are trademarks of Yamaha Corporation. AV Dexed is not affiliated with Yamaha.

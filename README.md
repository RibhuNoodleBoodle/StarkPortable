# StarkPortable for PSP

StarkPortable is a ScummVM-based PSP port focused on playing The Longest Journey using the STARK engine. It uses the PSP Graphics Engine to render a 320×240 scene, presented on the 480×272 display, while retaining the game's 640×480 coordinates. You need your own game data.

The native renderer and larger text are under active development. Gameplay, text layout, performance and memory behavior still need validation across the game and on PSP hardware.

## Installation

Copy the packaged PSP application and its data files into a folder under `PSP/GAME` on the memory stick. Copy your game data to the memory stick, start the application and use **Add Game** to select the game directory. Settings are stored in `ms0:/residualvm.ini`.

The PSP controls and additional installation details are in [README.PSP](backends/platform/psp/README.PSP).

## Build

Install the [PSPDEV toolchain](https://github.com/pspdev/pspdev), including its target libraries. Set `PSPDEV` to the installation directory and put `PSPDEV/bin` on `PATH`. The native STARK lab helper uses an external build directory and groups PSP imports for this toolchain.

```sh
export PSPDEV=/path/to/pspdev
export LAB=/absolute/path/to/a/private/build-directory
JOBS=4 bash gemini_hydrogen/tools/build-psp-lab.sh native
```

The executable is written to `$LAB/build-native/EBOOT.PBP`. The helper fetches two missing upstream headers into the external build directory when needed. A PSP release bundle can be generated from that build directory using `make psp_release`, with the PSPDEV environment set as above. Keep game data, saves, SDK installations and build artifacts outside Git.

## Project history and licensing

See [handoff.txt](handoff.txt) for implementation and validation history. ScummVM and its contributors are credited in [AUTHORS](AUTHORS). This project is distributed under the GNU General Public License and other licenses in [LICENSES](LICENSES). See [COPYING](COPYING) for details.

# Comanche CD native preview

An experimental C++17/SDL2 reimplementation using your original Comanche CD
installation and CD payloads. Start with Comanche Training, Oil Tank Holiday.
That mission has a retained DOSBox-X comparison through objectives, debrief
and pilot save. Five additional completed routes cover Flying Werewolves,
Deadman's Grotto, Wing Leader, Siberian Chill and The Kilauea Encounter, with
matching declared state, RNG, debrief, saves and mixed audio. All 100 missions
have bounded native checks. Other selectable missions remain experimental.

## Run

Extract the entire archive. Windows needs its supplied SDL2.dll beside the
executable and the Windows Universal C Runtime. Both binaries target x86-64.
The Linux binary requires glibc 2.29 or later, libstdc++ providing
GLIBCXX_3.4.26, and an installed SDL2 runtime, version 2.0.10 or later.
Linux qualification currently uses Ubuntu 20.04 under WSL.

Provide the installed DOS game directory containing COMANCHE.EXE, COMANCHE.NAM
and campaign MIS files, and the COMANCHE payload directory from your CD.
Use the original uppercase resource filenames on case-sensitive filesystems.
The supported COMANCHE.EXE is the 67,221-byte packed CD specimen with SHA-256
`1fb8ec4023d8727a644685d49be64ca608bc2bdc12431cb4a00d6a0e8e653f4d`.

From PowerShell in the extracted directory, use your own paths.

```powershell
./comanche_viewer.exe "D:/Games/Comanche" "E:/COMANCHE" "./profile"
```

On Linux, use the corresponding directories.

```sh
./comanche_viewer /path/to/installed/COMANCHE /path/to/cd/COMANCHE ./profile
```

The separate profile directory stores COMANCHE.NAM and COMANCHE.MNU. Existing
records receive a .bak before replacement. Keep that directory outside the
original installation and retain backups of valued profiles. A first launch
uses the original installation's pilot/settings defaults.

## Controls

| Context | Input | Action |
|---|---|---|
| Menus | Arrows, Enter, Escape | Navigate, accept, return/settings |
| Pilot name | Letters, Shift, Backspace, Enter | Edit and accept |
| Flight | Arrows | Steer |
| Flight | E / D | Increase / decrease throttle |
| Flight | Tab / Enter | Acquire or cycle target |
| Flight | [ / ] | Previous / next weapon |
| Flight | Space | Fire, hold for cannon |
| Flight | Semicolon / apostrophe | Chaff / flare |
| Flight | F1–F6 / F7–F12 | Left / right instrument display |
| Flight | 1 / 6 / 7 / 8 | Cockpit / chase / fixed / fixed view |
| Session | P | Pause/resume, outside pilot name entry |
| Flight | Escape | Open in-flight settings |
| Flight | Alt-A | Abort to debrief |
| Debrief | Escape | Return to operations |
| Window | Close | Exit |

Focus loss pauses and releases held controls. Resume with P after returning.
The default faithful input profile uses the keyboard. Views 2–5, J calibration,
the M shortcut and legacy input-mode/peripheral settings are unavailable in this
preview. They display a notice in the window title and retain the session.
Use [ / ] and Space for the supported weapon selection and firing path.
These notices are preview behavior. The corresponding DOS children still need
recovery. Supported commands clear the notice.

## Enhanced SDL2 input

Append `--mode enhanced` to enable SDL2 GameController input. Both input profiles
use the same simulation and indexed renderer. Enhanced input adds analog
steering and buffers short action presses until the next simulation tick.

```sh
./comanche_viewer /path/to/installed/COMANCHE /path/to/cd/COMANCHE ./profile --mode enhanced
```

| Context | Controller input | Action |
|---|---|---|
| Menus | D-pad, A, B | Navigate, accept, return/settings |
| Flight | Left stick | Steer, with a per-axis deadzone |
| Flight | D-pad Up / Down | Increase / decrease throttle |
| Flight | A / right trigger | Fire |
| Flight | Y | Acquire target |
| Flight | X / B | Chaff / flare |
| Flight | Left / right shoulder | Previous / next weapon |
| Flight | Back | Open settings |
| Session | Start | Pause/resume outside pilot name entry |

Pilot names use the keyboard. Disconnect pauses flight. A held menu button
requires release before becoming a flight action. Returning focus or changing
context requires neutral steering/trigger axes. An optional
`--controller-mappings FILE` loads a platform-filtered SDL2 mapping database.
The original DOS joystick settings remain separate from this host input
profile. Physical controller qualification remains open. The mapping boundary
uses SDL2's GameController API. [SDL2 controller documentation](https://wiki.libsdl.org/SDL2/SDL_GameControllerOpen)

## Optional OpenXR admission

`COMANCHECD_BUILD_OPENXR=ON` builds the optional loader and runtime/graphics
admission checks. Supply an independently installed OpenXR SDK include directory.

```sh
cmake -S source -B build -DCOMANCHECD_BUILD_OPENXR=ON -DCOMANCHECD_OPENXR_INCLUDE_DIR=/path/to/OpenXR-SDK/include
cmake --build build --target comanche_viewer
./build/comanche_viewer --openxr-info
```

The query reports runtime identity and headset OpenGL requirements. Runtime
admission has synthetic lifecycle/error coverage. Headset frame submission,
spatial terrain/entity snapshots, stereo rendering, XR actions and physical
headset qualification remain planned. The current immutable session snapshot
and latest-frame mailbox provide the presentation boundary for that work.
Graphics requirements are queried before creating an XR graphics session.
[Khronos OpenXR requirements](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetOpenGLGraphicsRequirementsKHR.html)

## Preview limits

- SDL2 prebuffers 100 ms of audio at startup and after clearing its queue.
  This host playback latency protects against renderer waits. The recovered
  CPU clock and generated PCM retain their original ordering.
- Flight pacing has a 20 Hz ceiling and accounts for longer recovered CPU/device
  work. Each frame drains its complete generated audio block. Presentation
  queue resets discard unplayed transition audio, including buffered loading
  music.
- The five completed comparison routes match their declared mixed-audio
  intervals. Siberian Chill matches all 2,296 observed IRQ0 entry timestamps
  from first flight loop through first debrief. These comparisons condition
  the initial clock state and retain their recorded input calendars.
- Oil Tank Holiday's continuous cold-entry audio, broader cold-menu timing,
  final-mission victory/unlock and alternate-campaign progression remain
  unqualified. Kilauea's loss comparison uses an initial progress fixture.
- Keyboard bindings above describe the native preview. Original command and
  modal coverage is incomplete. Some settings effects remain unverified.
- Rendering fidelity outside retained routes and broader hardware testing
  remain open.

When reporting a problem, include the archive version, OS, selected campaign
and mission, steps, and the console error. Keep original assets and personal
pilot files local.

## Source and notices

The archive includes matching source under source/. Build it with CMake 3.16+,
a C++17 compiler and SDL2 development files.

```sh
cmake -S source -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target comanche_viewer
```

When SDL2 is outside CMake's search path, add
`-DSDL2_DIR=/path/to/SDL2/cmake` to the configure command. Use the development
package matching your compiler and architecture.

Original project code retains the MIT notice in LICENSE-MIT. The application
links GPL-2.0-or-later DBOPL and adapted DOSBox-X audio code. COPYING and
source/THIRD_PARTY.md describe those components. SDL2's notice is SDL2-LICENSE.txt.
Source includes the retained third-party notices and build files.

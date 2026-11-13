# Third-party code

The linked FM synthesizer is DBOPL, Copyright (C) 2002–2021 The DOSBox Team,
licensed under GPL-2.0-or-later. Its license and source provenance are in
[third_party/dbopl](third_party/dbopl/README.md). The portable core removes
emulator callbacks and serialization; synthesis is unchanged.

`src/callback_audio.cpp` adapts channel interpolation, padding and filter
arithmetic from DOSBox-X `src/hardware/mixer.cpp`, under the same copyright and
GPL-2.0-or-later license. The license text is retained with DBOPL. This component
has no emulator or host dependency; it consumes explicit callback events.

`src/dsp_timing.hpp` adapts SB16 busy-phase arithmetic from DOSBox-X
`src/hardware/sblaster.cpp`, under the same copyright and GPL-2.0-or-later
license. The scheduler uses the retained profile's I/O and command-busy delays.
Its normal-core slice handling and VGA retrace profile also follow DOSBox-X.

`src/rtc_timing.hpp` adapts the update-only CMOS timer arithmetic from DOSBox-X
`src/hardware/cmos.cpp`, Copyright (C) 2002–2021 The DOSBox Team, under
GPL-2.0-or-later. It models scheduled CPU-slice timing for the retained device
profile. The license text is retained with DBOPL.

The native application links this GPL component. Distribution must include its
license and satisfy the corresponding source requirements. The generic
dos-re-harness license does not replace this component's license.

The earlier BSD-licensed YMFM source remains in `third_party/ymfm` for historical
reference; the current build does not link it. SDL2 remains the desktop host
dependency. Original game data is user-supplied and is not included in source
or native build archives.

The Windows preview statically links GCC and MinGW-w64 runtime libraries.
Pinned toolchain license texts and provenance are retained in
[third_party/toolchain](third_party/toolchain/README.md). SDL2's notice is
retained in [third_party/SDL2-LICENSE.txt](third_party/SDL2-LICENSE.txt).

Optional OpenXR admission support uses independently installed Khronos OpenXR
SDK headers and the runtime's standard loader through SDL2. SDK headers and
runtime libraries are external dependencies and are excluded from this archive.

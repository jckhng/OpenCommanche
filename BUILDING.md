# Building OpenCommanche

Use CMake 3.16+, a C++17 compiler and SDL2 2.0.10+ development files. Obtain
dependencies separately through your toolchain or package manager.
The source root contains CMakeLists.txt. CMake uses local dependencies.

## Windows x64

The RC10 Windows toolchain is MinGW-w64 GCC 13.2.0 with SDL2 2.32.10.
Use the SDL2 development package matching your compiler and architecture.
Place CMake and the compiler tools on PATH. Set `SDL2_DIR` to your SDK's
directory containing SDL2Config.cmake.

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DSDL2_DIR="$env:SDL2_DIR" `
  -DCOMANCHECD_BUILD_DIAGNOSTICS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake copies SDL2.dll beside comanche_viewer.exe when using its imported SDL2
target. The MinGW player statically links its compiler runtimes. Keep the
shared SDL2 DLL beside the player.

```powershell
./build/comanche_viewer.exe "D:/Games/Comanche" "E:/COMANCHE" "./profile"
```

## Linux x86-64

RC10 Linux qualification uses GCC 9.4 and SDL2 2.0.10 on Ubuntu 20.04 under WSL.
Install your distribution's C++ toolchain, CMake, pkg-config and SDL2 development
package. On Debian/Ubuntu, the package names are build-essential, cmake,
pkg-config and libsdl2-dev.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCOMANCHECD_BUILD_DIAGNOSTICS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/comanche_viewer /path/to/installed/COMANCHE /path/to/cd/COMANCHE ./profile
```

The runtime uses the installed SDL2 and C++ libraries. Original resources keep
their uppercase filenames on case-sensitive filesystems.

## Tests and installation

CTest runs the resource-free suites. Windows passes 45 suites. Linux passes
44, with an existing controller test skip on SDL2 versions lacking its virtual
controller support.

Optional integration checks read your own supported game data. Run from the
repository root, substituting your directories. Use `.exe` on Windows.

```sh
./build/viewer_tests /path/to/installed/COMANCHE /path/to/cd/COMANCHE
./build/session_tests /path/to/installed/COMANCHE /path/to/cd/COMANCHE
```

Installation produces the player, notices and matching source.

```sh
cmake --install build --prefix dist/OpenCommanche
```

Use `-DBUILD_TESTING=OFF` for a player-only build. Set
`COMANCHECD_BUILD_DIAGNOSTICS=ON` to build the retained diagnostic tools.
Build and test artifacts belong in ignored output directories.

## Optional input and OpenXR

Append `--mode enhanced` when launching to enable experimental SDL2
GameController input. Physical controller qualification remains open.

OpenXR support currently covers loader/runtime admission queries. Headset frame
submission and OpenXR gameplay remain planned. The optional build requires
independently installed Khronos SDK headers. See PREVIEW.md for its CMake
options and runtime query. The initial release defaults to SDL2 keyboard play.

## Source and redistribution

The RC10 source files retain their release-archive bytes. SOURCE_MANIFEST.json
lists repository file hashes, excluding the manifest itself, and the two
qualified runtime archive hashes. The GitHub RC10 tag also supplies a source
download. Runtime ZIPs each contain their matching source and notices.

Player-supplied game files, native profiles, captures, SDKs and local build
configuration stay outside the published tree. Preserve LICENSE-MIT, COPYING
and the third-party notices when redistributing source or binaries.

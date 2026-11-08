# Building

The supported build uses Linux, CMake 3.24+, Ninja, MinGW-w64, `clang-cl` and OpenXR SDK headers. NVIDIA headers are external dependencies and are not included in this repository.

## Windows addon

Provide the NVIDIA common NGX, Neural Rendering and DLSS Frame Generation header directories:

```sh
cmake -S . -B build-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENXR_INCLUDE_DIR=/usr/include/openxr \
  -DNGX_COMMON_INCLUDE=/path/to/ngx-common/include \
  -DNGX_NR_INCLUDE=/path/to/neural-rendering/include \
  -DNGX_FG_INCLUDE=/path/to/frame-generation/include
cmake --build build-win --target dist_package
```

The output is `build-win/Local-TF2VRNeuralRendering-0.1.0.zip`. NVIDIA runtime DLLs must be installed separately.

Without both common and NR include paths, the build contains capture support only. Omitting `NGX_FG_INCLUDE` omits frame generation. The menu is shared by these builds; use all three paths for the complete addon.

The NGX parameter helper is built with clang-cl for NVIDIA's MSVC virtual-interface ABI. The remaining code uses MinGW. The driver-core lifecycle code has been tested with NVIDIA 615.78.08; different driver-core ABIs may need adaptation.

## Portable tests

Install a C++17 compiler, CMake, Ninja and OpenXR headers, then run:

```sh
cmake -S . -B build-native -G Ninja
cmake --build build-native
ctest --test-dir build-native --output-on-failure
```

These cover frame lifecycle, OpenXR routing, stereo frame delivery, and menu settings synchronization. They do not start the game or require a headset.

The Windows targets `menu_config_tests` and `menu_bridge_probe` check INI persistence and the ConVar bridge. Run them only in a separate test Wine/Proton prefix. The bridge probe takes the path to the installed game's `engine.dll`; it maps that file without starting Titanfall or EA. Its adjacent `vstdlib.dll` is a test fixture and must never be installed with the addon.

GPU probes are developer tools, not part of the release package. The actual menu and VR image quality require manual in-game testing.

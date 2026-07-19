# ENet (vendored dependency)

KenshiCoop uses [ENet](http://enet.bespin.org/) for UDP networking (reliable +
unreliable channels). ENet is a
small, portable C library that compiles cleanly under both the VS2010 (v100)
plugin toolchain and modern compilers.

We do not check ENet's source into this repo. Fetch it into this folder one of
two ways.

## Option 1: git submodule (recommended)
```bash
git submodule add https://github.com/lsalzman/enet.git third_party/enet/enet
git submodule update --init --recursive
```
This yields `third_party/enet/enet/include/enet/enet.h` and the `.c` sources.

## Option 2: manual download
Download a release from https://github.com/lsalzman/enet/releases and extract so
that the headers live at `third_party/enet/enet/include/enet/enet.h`.

## Build notes
- The `nettest` CMake build (`src/nettest/CMakeLists.txt`) compiles ENet from
  `third_party/enet/enet` directly, so no separate install is needed.
- For the plugin (`KenshiCoop.dll`), add ENet's `include/` to the project includes
  and either add the ENet `.c` files to the project or link a prebuilt
  `enet.lib`. On Windows, ENet also needs `ws2_32.lib` and `winmm.lib`.
- Define `ENET_STATIC` (or build ENet as part of the project) to avoid DLL export
  issues when statically linking.

# Build and install VMHook

VMHook targets Windows x64 with MinGW-w64 GNU G++. The standalone header stays
C++23 and requires G++ 14 or newer. The named module requires G++ 16's C++26
reflection, `import std`, CMake 4.0 or newer, and Python 3.10 or newer.

## Install from source

Run these commands in a Windows shell with the chosen compiler on `PATH`:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTING=OFF
cmake --install build --prefix C:/libraries/vmhook
```

Installation includes the header, module sources, the Windows SDK boundary,
module build tools, CMake package and the current license. Building the module
in the producer checkout is optional. The consumer always builds its own
module artifacts from the installed sources.

An independent header project uses:

```cmake
cmake_minimum_required(VERSION 3.28)
project(payload LANGUAGES CXX)
find_package(vmhook CONFIG REQUIRED COMPONENTS header)
add_library(payload SHARED payload.cpp)
target_link_libraries(payload PRIVATE vmhook::vmhook)
```

A module project uses:

```cmake
cmake_minimum_required(VERSION 4.0)
project(payload LANGUAGES CXX)
find_package(vmhook CONFIG REQUIRED COMPONENTS module)
add_library(payload SHARED payload.cpp)
target_link_libraries(payload PRIVATE vmhook::module)
```

Configure either project with `-DCMAKE_PREFIX_PATH=C:/libraries/vmhook`.
The source file uses `#include <vmhook/vmhook.hpp>` for the header, or
`import std; import vmhook;` for the module. A payload may opt into
`target_link_options(payload PRIVATE -static)` to avoid compiler-runtime DLL
search-path dependencies.

## Compiler options and module artifacts

GNU CMIs depend on the compiler and compilation options. VMHook builds the
standard-library module, SDK header unit and named modules in dependency
order in the **consuming build directory**. It never imports a producer's
cached `.gcm` file. Release and Debug configurations have separate directories.

Set global `CMAKE_CXX_FLAGS` and configuration flags before `find_package`.
Options shared specifically by VMHook and its importers can be supplied as a
CMake list in `VMHOOK_MODULE_COMPILE_OPTIONS`; for example:

```cmake
set(VMHOOK_MODULE_COMPILE_OPTIONS "-O2;-D_GLIBCXX_ASSERTIONS")
find_package(vmhook CONFIG REQUIRED COMPONENTS module)
```

Avoid changing standard-library ABI, exception, runtime or module-affecting
options on just one importing target. Configure a separate consumer build
with the desired common options. Warning options on a target need not match
the module's warning options. `VMHOOK_STD_MODULE_SOURCE` provides an explicit
override when the compiler's `libstdc++.modules.json` needs one.

For affected GNU Windows builds, the orchestrator creates a libstdc++ header
overlay inside this build directory to preserve the shared-pointer COMDAT
symbol. A second narrowly matched GNU 16 overlay preserves constexpr exception
copy/move constructor linkage in unoptimized static builds. The compiler
installation remains untouched. The generated
`vmhook-module/<configuration>/build-info.json` records compiler version and
target, options, source hashes, artifact hashes and overlay use. These are
build facts; runtime support still requires the exact-image checks described
in [native threads](native-threads.md).

## Verify an installation

The repository's independent consumer has two translation units and exercises
owning Java-null wrappers, no-JVM refusal, `std::filesystem` and `std::future`.
It is copied outside the checkout and uses only `find_package`:

```sh
python tools/verify_installed_package.py --prefix C:/libraries/vmhook --work-dir C:/tmp/consumer-header --compiler g++
python tools/verify_installed_package.py --prefix C:/libraries/vmhook --work-dir C:/tmp/consumer-module --compiler g++ --module
```

Use an empty work directory for each run. Windows CI also rebuilds the
installed module in Debug with a changed common compiler definition.
This consumer verifies packaging, linking and ownership without a JVM;
the separate [standalone examples](../examples/README.md) verify live Java use.

## Source archives and licensing

On a clean, committed revision:

```sh
python tools/package_source.py --output C:/tmp/vmhook-release
```

The deterministic source ZIP uses fixed entry metadata and uncompressed entries
so its bytes do not depend on a compression-library version. It includes all
tracked source files and an internal
`SOURCE-MANIFEST.json`; a matching manifest and `SHA256SUMS` are emitted beside
it. The manifest records the exact Git commit/tree and every source-file hash.
Version metadata and generated-header consistency are checked before creation.
The package contains no compiled module caches or native binaries. Publishing
a GitHub release is a separate repository operation after validation.

VMHook is licensed under **PolyForm Noncommercial 1.0.0**, identified in the
vcpkg metadata and included with the installation and archive. The existing
license does not grant general commercial use; consult [LICENSE.txt](../LICENSE.txt)
for its permitted purposes and full terms. A separate commercial grant, if
offered, must come from the copyright holder. Packaging does not change those
terms.

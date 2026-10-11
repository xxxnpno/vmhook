# Reproducible runtime benchmarks

`benchmarks/runtime.cpp` is an executable DLL workload for VMHook's actual
native-thread API. It measures attach/detach, field lookup/read, method
lookup/call, a retained method lookup, UTF-8 String conversion and conversion
of a three-element collection, including a Java-null wrapper.

Build it from an installation:

```sh
cmake -S benchmarks -B build/benchmarks -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_PREFIX_PATH=C:/libraries/vmhook -DCMAKE_BUILD_TYPE=Release
cmake --build build/benchmarks
```

Use `-DVMHOOK_BENCHMARK_MODULE=ON` for the C++26 interface. Repository builds
enable it with `-DVMHOOK_BUILD_BENCHMARKS=ON` and select the module separately.

Run on native Windows with the ordinary Java fixture:

```sh
python tools/run_examples.py --java C:/jdk/bin/java.exe --javac C:/jdk/bin/javac.exe --injector build/tests/vmhook_test_injector.exe --binaries build/examples --benchmark build/benchmarks/vmhook_benchmark.dll --iterations 1000 --evidence build/benchmark.json
```

Each operation produces a sample count and p50/p99 wall times in nanoseconds
using `std::chrono::steady_clock`. Inputs/results are consumed, and Java call
and collection failures abort the workload. The evidence records DLL hash,
Java launcher hash, execution environment, iteration count and results.
Module builds additionally record compiler/options/source hashes in their
build manifest. Capture the host CPU, Windows version, power settings and
background load alongside a performance comparison.

These measurements include clock overhead and the particular fixture's Java
work. Lookup/call is a composite operation; the retained-lookup case isolates
lookup reuse. Collection samples include allocation of C++ wrappers and their
context-bound roots. Context samples include repeated registration and closure;
their first sample also includes cold backend discovery. No result is a
promise for another application, collector, compiler or JVM image.

The initial suite does not measure whole-application throughput, deoptimization
pauses, long-running root growth, contention, heap usage or another bridge.
Add such workloads with explicit correctness checks before making those
comparisons. CI captures observations rather than enforcing timing thresholds:
shared runner timings vary. Wine checks verify executable behavior and are
identified as emulator evidence; use native Windows for published performance
figures.

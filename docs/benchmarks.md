# Runtime measurements and GC soak

The executable benchmark DLL runs on a native worker in an ordinary, warmed
Java process. It exercises VMHook rather than timing an empty substitute.

| Workload | Recorded evidence |
| --- | --- |
| Context registration and closure | First attachment and repeated attach/detach wall times |
| Field/method access, cached lookup, String and list conversion | First invocation and repeated p50/p99 wall times |
| Object construction and reference-field write/read | Checked Java values and rooted object identity |
| Method hook installation | Installer wall time, entry counts, observer/policy/stub resources |
| Java target and unrelated loops | Before, active-hook and after-shutdown wall times |
| JIT transitions | Target/caller/unrelated compiled state, target/unrelated exclusion at safepoints |
| Repeated GC with a live native context | Rooted constructor, field and list results after Java clears its references |
| Context closure and durable retention | Fresh-context acquisition, Java value and identity after collection |
| Runtime resources | Java heap/non-heap usage, real GC counters, thread count, compiler time, root slots/leases and hook resources |

Build from an installation:

~~~sh
cmake -S benchmarks -B build/benchmarks -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_PREFIX_PATH=C:/libraries/vmhook -DCMAKE_BUILD_TYPE=Release
cmake --build build/benchmarks
~~~

Use -DVMHOOK_BENCHMARK_MODULE=ON for the C++26 interface. Repository builds
enable -DVMHOOK_BUILD_BENCHMARKS=ON.

Run a short characterization on native Windows:

~~~sh
python tools/run_examples.py --java C:/jdk/bin/java.exe --javac C:/jdk/bin/javac.exe --injector build/tests/vmhook_test_injector.exe --binaries build/examples --benchmark build/benchmarks/vmhook_benchmark.dll --iterations 1000 --soak-cycles 32 --soak-batch 16 --evidence build/benchmark.json
python tools/summarize_benchmark.py build/benchmark.json --output build/benchmark-summary.json
~~~

For a longer fixed-input soak, increase --soak-cycles and --soak-batch,
for example --soak-cycles 1000 --soak-batch 64 --timeout 3600. Each cycle
constructs a bounded batch, sets a reference field, appends to a Java list,
copies list owners and independently retains the last object. Java then
clears both links and requests GC through an ordinary method invocation.
Every constructor/field/list result is checked afterwards, including allocated
C++ wrappers for Java-null values. Real collector counts must increase.
The worker keeps its native context open throughout the cycle workload.
After the first cycle, durable allocated capacity/lease counts and context
allocated capacity/live ticket counts must remain unchanged in each subsequent
cycle. A normal Java operation and a temporary retain explicitly drain deferred
releases before these comparisons; the diagnostic getter itself never drains.
A separate durable object is acquired after that context closes and another
context triggers collection.

The first context sample includes backend discovery. Other first samples
reuse it; they are labelled first invocation rather than cold JVM execution.
Repeated p50/p99 exclude that first sample. Timed construction includes a
field read to validate the result; reference-field timing includes identity
validation. All samples include the clock and bridge overhead. The Java
loop performs 1,024 operations per sample and includes one bridge call.
The active hook records entry counts using a relaxed atomic counter.

Installing a method policy currently performs **VM-wide compiled-code
deoptimization** and excludes its target from compilation/inlining. The
benchmark verifies that all three fixture methods were compiled before
installation. It then records their actual states at safepoints. Unrelated
methods can recompile; target exclusion lasts through observer removal until
policy shutdown. Shared observer add/remove timings reuse the existing
physical hook and therefore do not represent first-time hook installation.
Installer wall time includes compiler coordination, deoptimization and code
publication; it is not a measurement of isolated VM safepoint duration.
The related loop ratios describe this fixed workload, not application-wide
overhead or a selective-deoptimization guarantee.

Heap usage and GC counters are sequential Java management observations.
They are not an atomic heap census, C++ process-memory measurement or proof
that every internal root slot was reclaimed. C++ owner creation counts are
explicitly labelled separately. Published hook return PCs require the DLL
to remain loaded even after observers and policies have been removed.

Evidence records the DLL, injector, fixture, Java launcher and JVM-image
hashes, host platform/CPU information, workload parameters and observations.
Module build manifests separately record compiler/options/source hashes.
For comparisons, preserve both manifests, use the same host power settings
and background load, and repeat in several fresh JVM processes. The summary
tool reports paired workload ratios without asserting statistical significance.

CI runs the short workload on Java 8, 21 and 27 for both interfaces and saves
results without timing thresholds. Wine runs are supplemental executable
checks, identified as provisional emulator evidence. Native Windows is
required for published performance conclusions. This suite does not measure
contention, virtual threads, original method-exit latency, mixed-stack sampling
or compare another Java bridge.

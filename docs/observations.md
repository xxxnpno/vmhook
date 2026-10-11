# Runtime and Java thread observations

With a live native context, VMHook can copy the JVM's standard management data:

```cpp
vmhook::native_thread_context context;
const auto runtime = vmhook::observe_runtime();
const auto threads = vmhook::observe_java_threads(64, 4096);
```

`observe_runtime()` returns heap and non-heap usage, uptime, class and thread
counts, compilation time when available, and each GC's collection count and
elapsed collection time. The capture interval brackets these sequential reads;
the result is not an atomic snapshot of all counters. An unavailable JVM
measurement is an empty `optional`, so it cannot be confused with a real zero.

`observe_java_threads()` returns copied names, IDs, states, blocking/waiting
counts, supported CPU times and Java stack frames. It uses `ThreadMXBean`, so
compiled and inlined Java frames follow the JVM's stack-trace rules. It does not
enable CPU or contention monitoring: disabled or unsupported measurements stay
unavailable. A thread terminating after enumeration has `alive == false`.
The depth and thread-count limits bound the requested capture; exceeding a
thread limit raises an error instead of silently truncating it.

These APIs execute ordinary Java calls through VMHook's rooted native backend.
They require no agent API or JVM launch settings. Bootstrap classes are loaded
through ordinary `Class.forName`; no hidden helper classes are injected.
All results are copied C++ data and remain usable after the context closes.
They are synchronous observations, not asynchronous CPU samples, mixed native
and Java stacks, exact heap graphs, or GC/allocation event callbacks.

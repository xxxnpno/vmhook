# Runtime and Java thread observations

With a live native context, VMHook can copy the JVM's standard management data:

```cpp
vmhook::native_thread_context context;
const auto runtime = vmhook::observe_runtime();
const auto threads = vmhook::observe_java_threads(64, 4096);
const auto pools = vmhook::observe_memory_pools();
const auto allocations = vmhook::observe_thread_allocations();
```

`observe_runtime()` returns heap and non-heap usage, uptime, class and thread
counts, compilation time when available, and each GC's collection count and
elapsed collection time. The capture interval brackets these sequential reads;
the result is not an atomic snapshot of all counters. An unavailable JVM
measurement is an empty `optional`, so it cannot be confused with a real zero.
Thread counts describe platform threads; virtual threads are excluded from the
live, daemon, peak and total-started thread counts.

`observe_memory_pools()` copies each pool's current, peak and post-collection
usage where available, plus supported threshold counters. Java may invalidate a
pool or return null usage during its lifetime; those usages stay unavailable.

`observe_thread_allocations()` queries the optional HotSpot management extension
on the actual thread bean. It reports whether counting is supported and enabled
and copies cumulative allocated bytes per thread. It does not enable counting.
Terminated threads may have unavailable counters. These bytes measure allocations,
not retained objects, and the sequential reads include an explicit capture interval.
Its inventory contains platform threads, following `ThreadMXBean` enumeration.

`observe_java_threads()` returns copied platform-thread names, IDs, states,
blocking/waiting counts, supported CPU times and Java stack frames. It also copies
the waited-on lock's class and identity hash, plus optional owner ID and name.
An unavailable owner is distinct from an owner whose name happens to be empty.
It uses `ThreadMXBean`, so
compiled and inlined Java frames follow the JVM's stack-trace rules. It does not
enable CPU or contention monitoring: disabled or unsupported measurements stay
unavailable. A thread terminating after enumeration has `alive == false`.
The depth and thread-count limits bound the requested capture; exceeding a
thread limit raises an error instead of silently truncating it.
Virtual threads are not enumerated, even while mounted. A carrier's record does
not establish the identity, stack or CPU time of a particular virtual thread.

## Deadlock and wait-owner snapshots

```cpp
vmhook::native_thread_context context;
const auto deadlocks = vmhook::observe_deadlocks();
for (const auto& thread : deadlocks.threads)
{
    if (thread.alive && thread.waiting_on && thread.lock_owner_id)
    {
        report_wait(thread.id, *thread.lock_owner_id,
                    thread.waiting_on->class_name, thread.waiting_on->identity_hash);
    }
}
```

The default `deadlock_scope::monitors_and_synchronizers` asks the JVM for cycles
involving object monitors and ownable synchronizers such as `ReentrantLock`.
`deadlock_scope::monitors` asks only for object-monitor cycles. An unsupported
synchronizer-aware detector raises `vmhook::exception`; it does not downgrade to
monitor-only detection. Normal contention with no cycle returns no deadlocks.
The operation uses ordinary Java management calls through VMHook's existing
rooted native backend. It requires a live native context and does not enable CPU
or contention monitoring, request an agent capability or change JVM flags.

```cpp
vmhook::deadlock_options options;
options.scope = vmhook::deadlock_scope::monitors;
options.max_threads = 32;
options.max_depth = 8;
const auto deadlocks = vmhook::observe_deadlocks(options);
```

The default limits are 256 copied thread records and 64 Java frames per record.
Thread limits must be between 1 and 65,536; frame depth must be between 0 and
1,024. Invalid limits or scope values are refused before any Java call. Depth
zero omits stack frames and still copies lock/owner information. The snapshot
retains the requested limits and scope. Deadlock records do not request per-thread
CPU time, so their `cpu_time_ns` remains unavailable. If the detector returns more IDs than
`max_threads`, a prefix in the JVM's unspecified order is copied:
`detected_thread_count` retains the full detected count, `thread_limit` is true
and `complete` is false. Omitted IDs are not silently interpreted as absent
cycles. The detector still performs its full search and creates its full Java
ID result; these budgets bound copied records and requested frames, not that
search, its allocations or VM pause duration.

A Java-null detector result is a complete empty snapshot. If `getThreadInfo`
returns Java null after detection, its ID remains in a record with `alive ==
false`, and `thread_info_raced` makes `complete` false. A non-null record means
its `ThreadInfo` was available when copied, rather than promising it is still
alive. `complete` describes copying all detected IDs under the requested depth;
it does not imply a globally consistent wait graph. Detection and each thread's
information are separate reads inside `capture_started`/`capture_finished`.
Threads, locks and cycles may change between them. Owner IDs/names are optional;
the lock's identity hash is diagnostic data, not a unique object ID or address.

Only platform-thread cycles are detected. Cycles involving virtual threads are
outside `ThreadMXBean`'s contract, including mixed platform/virtual cycles.
No Java objects or raw object addresses escape in the copied snapshot; its
strings, IDs and stack frames remain usable after the native context closes.
Checked C++ allocation failures propagate their original exception while VM
state unwinds normally. As with other rooted operations, `noexcept` root-release
or recycling bookkeeping may absorb an allocation failure and keep a scanned
slot until context closure. The snapshot does not add a stronger release policy.

The owned `DeadlockFixture` starts with ordinary noncyclic contention and then
creates real daemon-owned monitor and `ReentrantLock` cycles. Its probe verifies
the two detector scopes, actual owner links and monitor identity hashes,
zero-depth and thread-count budgets, copied lifetimes and allocation-failure
cleanup/retry. Deterministic proxy-bean responses separately exercise unsupported
synchronizer scope and a disappearing `ThreadInfo`; these seams are not reported
as real application deadlock races. On Java 21+, an ordinary virtual thread
confirms the documented platform-thread inventory boundary. No deadlocked thread
is forcefully stopped or joined; the harness terminates its own JVM afterward.

The [upstream ThreadMXBean contract](https://github.com/openjdk/jdk/blob/815ff4dc327fe17f2433c7d115a5a503af10f3c4/src/java.management/share/classes/java/lang/management/ThreadMXBean.java#L34)
defines the platform-thread boundary and separate
[monitor/synchronizer detector](https://github.com/openjdk/jdk/blob/815ff4dc327fe17f2433c7d115a5a503af10f3c4/src/java.management/share/classes/java/lang/management/ThreadMXBean.java#L609).

These APIs execute ordinary Java calls through VMHook's rooted native backend.
They require no agent API or JVM launch settings. Bootstrap classes are loaded
through ordinary `Class.forName`; no hidden helper classes are injected.
All results are copied C++ data and remain usable after the context closes.
They are synchronous observations, not asynchronous CPU samples, mixed native
and Java stacks, exact heap graphs, or GC/allocation event callbacks.

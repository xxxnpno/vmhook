# Java monitors

`monitor_lock` acquires the actual Java object's monitor. It interoperates with
Java `synchronized` and `Thread.holdsLock`, including reentrant acquisition.
HotSpot performs the locking and unlocking through the exact profile's audited
private `ObjectLocker` constructor and destructor. VMHook does not reproduce
the collector's or JVM's lock-word algorithm.

```cpp
vmhook::native_thread_context context{"VMHook-worker"};
auto player = obtain_player(); // An owning wrapper rooted in this context.
{
    vmhook::monitor_lock lock{player};
    player->set_score(42);
} // Releases this acquisition, including when C++ code throws.

const auto score = vmhook::with_monitor(player, [&] { return player->score(); });
```

The wrapper must have a non-null Java instance and a live root in the current
standalone native context. An allocated Java-null wrapper raises
`null_reference`; an empty C++ owner is refused. Acquiring a monitor can block
until Java releases it. C++ code between operations remains in native thread
state, allowing GC and safepoints to proceed while the monitor stays held.
Calls made inside that interval enter and leave the VM normally.

The lock must reside on the acquiring OS thread's stack. It cannot be copied,
moved, stored on the heap or released on another thread. Keep its native context
alive until release: `context.close()` refuses closure while a monitor is held.
`unlock()` releases explicitly and is idempotent; `owns_lock()` reports whether
this C++ guard still owns its acquisition. Destruction releases the acquisition.
Failure to obey the owner/context lifetime at destruction terminates rather
than invoking a private destructor on a stale thread or lock record.

`monitor_locks_supported()` reports an available audited ABI. Passive
`runtime_capabilities().support(runtime_feature::java_monitors)` reports the
same backend as conditional because object, context and stack checks still
apply at acquisition. Exact profiles cover the changing `ObjectLocker` storage
and constructor signatures for Java 8–27 on the verified G1/Parallel images.
Other images, collectors and borrowed hook contexts are refused.

The runtime fixture checks Java-to-C++ and C++-to-Java contention, ordinary
field visibility, reentrancy, moving GC while held and while a native contender
waits, explicit release, context-close refusal and exception unwinding. Java
monitor `wait`/`notify`, timeouts, cancellation and virtual-thread monitor
ownership are outside this API's current contract. Use normal Java methods for
Java-level condition waiting.

The acquisition error path clears the JVM pending-exception slot even if C++
allocation fails while retaining or describing its Throwable. The lock is
balanced before propagating the C++ error. `monitor_fault_probe.cpp` exercises
that narrow failure by injecting a pending result after the clean preflight
and failing the real shared-root allocation; it then checks the pending slot,
monitor ownership, and successful acquisition on the same context.

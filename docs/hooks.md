# Persistent observers and event capture

`hook<T>(name, descriptor, callback)` remains a boolean convenience. Use
`add_hook<T>` when a registration needs an ID for explicit removal, priorities,
recursion policy or per-observer counters. Discarding an ID keeps its observer
installed. There is no scoped method hook.

C++ allocation failures during hook root and policy preparation propagate as
`std::bad_alloc`. Field-policy preparation restores compiler flags before
reporting its captured allocation error to the native caller. VM callbacks
contain C++ exceptions at their ABI boundary.

```cpp
const auto id = vmhook::add_hook<player>("tick", "(I)V",
    [](vmhook::return_value& control,
       const std::unique_ptr<player>& self, std::int32_t amount)
    {
        if (amount < 0) { control.cancel(); }
    }, {.priority = 10});

vmhook::remove_hook(id);
vmhook::reclaim_hook_callbacks();
```

Observers run in ascending priority, then registration ID. They share a physical
interpreter patch and a compilation/deoptimization policy for each method. The
first callback that cancels execution or sets a forced return wins; later
callbacks are skipped. A callback failure cancels its own forced return and is
counted. Argument writes already made by that callback are not rolled back.

These callbacks observe method **entry**. `return_value::set()` chooses an early
return; it does not report the original body's result or its duration.

An installation retains the exact declaring class and the wrapper bindings used
by its callback arguments, including the leaf wrappers of nested Java arrays.
Re-registering those C++ wrapper types affects future installations; an existing
callback keeps its captured classes for argument wrapping and construction.
Nested callbacks select their own captured bindings, then restore the outer
callback's bindings before it resumes. Native code outside callbacks uses the
current registrations.

Typed owning arguments are admitted only when they can represent the declared
Java parameter or receiver in its defining loader. Identical class names from
different loaders do not qualify. A wrapper for a verified superclass can accept
a declared subclass; the reverse direction is refused because other values of
the declared superclass would fail materialization. `any_object` retains its
opaque reference behavior. Java null still produces an allocated C++ wrapper
whose `get_instance()` is null.

`control.throw_exception(throwable)` forwards an allocated, rooted
`std::unique_ptr<vmhook::any_object>` containing a non-null Java `Throwable`.
It skips the original body and uses the genuine interpreter exception
continuation, so Java catches and `finally` blocks see the same object.
The pending exception takes precedence over an early return. Unrelated objects,
Java null and calls outside the active method callback are refused. Nested Java
calls and GC preserve the outer synchronized entry's monitor unwind flag.

Same-observer recursion is suppressed by default. Other observers can still
observe a nested invocation. `hook_recursion::allow` permits explicit recursion
with a maximum C++ dispatch depth of 64. `call_without_hooks(callable)` suppresses
method observers on the calling OS thread while invoking the callable; field and
class observers retain their normal behavior.

An observer selected by an in-flight immutable reader can finish during removal.
Callback-side removal only changes an atomic flag, avoiding a wait for an
administrator requesting a VM safepoint. `reclaim_hook_callbacks()` releases
retired captures from a native owner when readers permit reclamation. Removing
the final observer keeps the dormant method policy and stub until
`shutdown_hooks()`. `observer_stats(id)`, `hook_resources()` and
`hook_callbacks_idle()` report registrations, calls, failures and in-flight
readers. Registry aggregate sizes are available only to native administrators;
`registry_sizes_available` distinguishes that case. Counters do not authorize DLL
unloading: published stub return PCs and the injecting DLL remain resident.
`resident_return_markers` and `resident_return_marker_bytes` report permanent
native marker nodes used by the forced-reference-return GC scanner. A thread
reuses its node at each previously observed frame address; new frame addresses
and previously unseen threads can increase this resident storage. The nodes
contain frame and metadata identity pointers, rather than Java object handles.
Removal and `shutdown_hooks()` do not reclaim them while the scanner can read
its published list. The byte count covers node payloads, excluding allocator
bookkeeping and the shared scanner stubs.

Mounted virtual-thread frames do not enter a VMHook borrowed callback context.
Method and field operations on those frames run their original Java behavior;
`rejected_hook_callback_frames()` counts these refused callback frames. Platform
Java threads, including `native_thread_context` workers, retain their verified
callback path. Unknown mounted-thread layouts and unsupported collectors refuse
callback setup before publishing a Java anchor.

## Class notifications

The original `class_load_event` borrows metadata only during its callback.
`copy_class_event(event)` copies a complete name into an owning C++ snapshot.
Its `event_id` identifies a notification, not a durable class-loader binding.
The snapshot contains no raw VM pointers.

For a callback that performs no allocation or export I/O, use a bounded queue:

```cpp
auto events = std::make_shared<vmhook::class_event_queue>(256);
const auto id = vmhook::on_class_loaded(events);

vmhook::class_load_record record{};
while (events->try_pop(record))
{
    // Export on this native owner, after the VM callback has returned.
    // record.name() is copied data; check record.truncated before exact matching.
}

vmhook::remove_class_observer(id);
vmhook::reclaim_class_observers();
```

The transport retains at most its capacity, drops full or repeatedly contended
writes, and counts drops. Names longer than 1,024 bytes carry `truncated=true`.
A slow exporter cannot block the Java loading callback. Notifications do not
replay previously loaded classes and do not prevent or rewrite definitions.

`bounded_event_queue<record_type>` also accepts other trivially copyable records.
For example, a method-entry observer can enqueue a method ID, OS thread ID and
primitive arguments for a native JSON exporter. Java objects must use the
library's owning/root-transfer contract; raw oops do not belong in this queue.

## Deferred installation

```cpp
vmhook::native_thread_context context{"observer-owner"};
const auto intent = vmhook::hook_when_loaded<player>("example/Player", "tick", "()V",
    [](vmhook::return_value&, const std::unique_ptr<player>& self) { /* ... */ });

// Run from this native owner once classes can be linked, or in response to its
// copied class-event stream. This never executes inside a class VM callback.
vmhook::process_pending_hooks();
const auto state = vmhook::deferred_hook_status(intent);

vmhook::remove_deferred_hook(intent);
vmhook::reclaim_deferred_hooks();
```

The class callback marks a request pending. Registration, metadata roots, JIT
policy and patching occur only in `process_pending_hooks()` on an active native
context. A definition may initially remain unlinked; `retry_deferred_hook(id)`
requests another native attempt. Status distinguishes waiting, installing,
installed, failed and cancelled intents. Native Java calls and constructors
execute directly and never depend on this queue or on a Java hook firing.
Shutdown disables outstanding intents. Ambiguous class names require an exact
loader binding; a copied notification ID cannot supply one.

## Throwable stack-trace observations

```cpp
const auto id = vmhook::on_throwable_stack_trace(
    [](const vmhook::throwable_stack_trace_event& event)
    {
        // event.throwable is an owning wrapper valid in this callback context.
        // event.internal_name is copied C++ text.
    });
vmhook::remove_hook(id);
```

This exact event observes entry to `Throwable.fillInStackTrace()`. Ordinary
writable-stack-trace constructors generally call it, and explicit refreshes also
call it. Repeated throws/catches of an existing exception do not notify through
this path. Exceptions with disabled or overridden stack-trace collection can
avoid it. The compatibility `on_exception` watcher retains these same limits;
it is not a general throw/catch event.

## Shared-class interpreter entries

The calibrated Java 12–16 builds can keep a shared method's `_i2i_entry` in a
CDS trampoline outside the interpreter codelets. VMHook accepts the observed
16-byte form `49 BA <imm64> 41 FF E2 00 00 00` only when its destination is the
independently validated normal or synchronized interpreter entry for that
method. Other trampoline forms remain unsupported. The three padding bytes
are checked runtime observations; the source reserves 16 bytes and emits a
13-byte far jump.

Installation decodes and patches the actual interpreter prologue, shares that
physical patch with other methods reaching the same entry, and rechecks the
destination at its commit safepoint. Its immutable callback snapshot retains the
actual interpreter PC for frame anchors during nested Java calls and GC.
It leaves both the CDS trampoline and shared Method entry fields intact, as
`Method::link_method` expects. Compilation exclusion and deoptimization use the
existing method policy. This support does not relax prologue decoding or the
mounted-virtual-thread refusal.

The exact source revisions defining these CDS jumps are:

| Calibrated Java | CDS entry generation | Shared Method linking |
| --- | --- | --- |
| 12 | [update_cds_entry_table](https://github.com/openjdk/jdk12u/blob/75d7a31bc5c7ed56da4fea525a3f4bf6206acf87/src/hotspot/share/interpreter/abstractInterpreter.cpp#L214) | [link_method](https://github.com/openjdk/jdk12u/blob/75d7a31bc5c7ed56da4fea525a3f4bf6206acf87/src/hotspot/share/oops/method.cpp#L1023) |
| 13 | [update_cds_entry_table](https://github.com/openjdk/jdk13u/blob/0a2532b9fe3feb683aad789b485415350b857fc5/src/hotspot/share/interpreter/abstractInterpreter.cpp#L217) | [link_method](https://github.com/openjdk/jdk13u/blob/0a2532b9fe3feb683aad789b485415350b857fc5/src/hotspot/share/oops/method.cpp#L1066) |
| 14 | [update_cds_entry_table](https://github.com/openjdk/jdk14u/blob/a718f22fc087a923ccb873ee76d8f0b2e6a16863/src/hotspot/share/interpreter/abstractInterpreter.cpp#L217) | [link_method](https://github.com/openjdk/jdk14u/blob/a718f22fc087a923ccb873ee76d8f0b2e6a16863/src/hotspot/share/oops/method.cpp#L1164) |
| 15 | [update_cds_entry_table](https://github.com/openjdk/jdk15u/blob/03fb8436dc5f86be1a9633658d6295b9dc04fa0e/src/hotspot/share/interpreter/abstractInterpreter.cpp#L212) | [link_method](https://github.com/openjdk/jdk15u/blob/03fb8436dc5f86be1a9633658d6295b9dc04fa0e/src/hotspot/share/oops/method.cpp#L1176) |
| 16 | [generate_entry_for_cds_method](https://github.com/adoptium/jdk16u/blob/3b56f0b15cee132d6366e8ec03e05c918a42b4f1/src/hotspot/share/interpreter/abstractInterpreter.cpp#L223) | [link_method](https://github.com/adoptium/jdk16u/blob/3b56f0b15cee132d6366e8ec03e05c918a42b4f1/src/hotspot/share/oops/method.cpp#L1199) |

[SharedRuntime::generate_trampoline](https://github.com/openjdk/jdk13u/blob/0a2532b9fe3feb683aad789b485415350b857fc5/src/hotspot/cpu/x86/sharedRuntime_x86_64.cpp#L407)
and [MacroAssembler::jump](https://github.com/openjdk/jdk13u/blob/0a2532b9fe3feb683aad789b485415350b857fc5/src/hotspot/cpu/x86/macroAssembler_x86.cpp#L3023)
describe the reserved slot and jump emission. `event_stream_probe` checks shared
non-synchronized `Integer.parseInt`, physical-patch reuse, original bodies and
removal, plus synchronized `Throwable.fillInStackTrace` with nested Java GC,
before-monitor callback timing and stack-trace refresh semantics.

## Reproduce the public observation fixtures

Configure with `VMHOOK_BUILD_RUNTIME_TESTS=ON` and build the DLLs and injector.
On Windows, run the normal Java launcher and the compiled observation probe:

```sh
python tools/run_observation_fixture.py --java path/to/jdk/bin/java.exe --javac path/to/jdk/bin/javac.exe --injector build/tests/vmhook_test_injector.exe --probe build/tests/vmhook_observation_probe.dll --mode all --evidence build/observation-results.json
```

The base probe checks actual JIT compilation/inlining, static primitive field
access and class definitions. Additional DLL arguments enable rooted callbacks,
observer composition, lifetime, management, executors, Throwable forwarding and
virtual-thread refusal; `--help` lists their exact names. Select the corresponding
`vmhook_module_*` DLLs to exercise `import vmhook` instead. The harness compiles
its Java fixtures, injects only its own JVM subprocess and checks every required
result. It never adds flags to disable compilation or change the collector.

`tests/runtime/run_reference_fields.py` and `tests/runtime/run_interface.py`
provide the separate reference-field and Java-interface callback fixtures. Their
evidence records include artifact hashes and partial results on failure. For
local Windows-binary checks on Linux, use the supported Wine/emulator option
where provided; those runs remain distinct from native Windows CI evidence.
The original exit and throw/catch backend sites, prototype limits and remaining
proof are described in [advanced-events.md](advanced-events.md).

# Persistent observers and event capture

`hook<T>(name, descriptor, callback)` remains a boolean convenience. Use
`add_hook<T>` when a registration needs an ID for explicit removal, priorities,
recursion policy or per-observer counters. Discarding an ID keeps its observer
installed. There is no scoped method hook.

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

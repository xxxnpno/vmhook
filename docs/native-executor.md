# Bounded native Java execution

`native_executor` owns a fixed number of DLL-created OS threads. Each worker
creates one `native_thread_context`, executes its tasks on that thread, and closes
its context before exiting. Java calls use the private HotSpot execution backend;
workers do not wait for an application method hook to service their requests.
The same audited JVM-image and collector requirements apply as for native
contexts, and durable object transfer additionally requires the root keeper
described in [Object retention](lifetime.md).

```cpp
vmhook::retained_object<player> saved;
{
    vmhook::native_thread_context source;
    saved = vmhook::retain(vmhook::make_unique<player>(std::int32_t{42}));
}

vmhook::native_executor executor{{.worker_count = 2, .queue_capacity = 64}};
auto task = executor.submit([](vmhook::retained_object<player> retained,
                               std::int32_t amount) -> std::int32_t
{
    auto value = retained.acquire();
    return value->get_method("add")->call(amount);
}, std::move(saved), std::int32_t{5});

const auto total = task.get();
executor.close();
```

The callable must be stateless or a function pointer. Pass owned input data as
explicit arguments: arithmetic values, enums, `std::string`, `std::u16string`,
`retained_object`, and recursively supported vectors, arrays, optionals, pairs,
and tuples. Vectors require their default `std::allocator`: a custom allocator
may itself carry borrowed state. Results use the same types, or `void`. Context-bound wrappers,
`std::unique_ptr<wrapper>`, references, raw pointers, and string views are rejected
as task arguments and results. C++23 cannot inspect arbitrary lambda captures;
requiring a stateless callable prevents a captured context-bound wrapper from
silently crossing threads. A task may create ordinary wrappers locally, but must
retain an object before returning it to another context. Java-null retained
results still acquire an allocated null wrapper.

`submit` reserves a queue slot before moving arguments. A full or closed queue
throws immediately and leaves retained arguments untouched. The queue capacity
counts pending entries and admission reservations; executing tasks have already
left it. A cancelled queued entry releases its slot when a worker consumes it.
C++ allocation failures propagate; arguments already moved during task
construction follow normal C++ exception semantics. `stats()` returns copied
queue and worker counts under the queue lock.

`submit_before` takes an absolute `std::chrono::steady_clock` deadline. A task
whose deadline expires while queued fails without calling its body. `cancel()`
likewise completes a queued task with an exception. Cancellation of a running
task is cooperative: accept `const native_task_control&` as the first parameter
and check `stop_requested()` or call `throw_if_cancelled()` between operations.
The executor checks cancellation and the deadline before and after the body.
It does not interrupt a running Java call, undo side effects, or roll back an
already completed operation. Exceptions thrown by the body are rethrown by `get()`. Use checked Java call
APIs inside a task when Java failures should become C++ exceptions; the executor
does not reinterpret unchecked call results.

A propagated `vmhook::exception` preserves its copied error code, diagnostic,
and Java exception class across the future. Its original Throwable root still
belongs to the worker's context: `get_throwable()` on the receiving thread
refuses that foreign root. The future does not automatically transfer it. When
the receiver needs the actual Throwable, catch the failure inside the worker,
call `retain(error.get_throwable())` there, and return the resulting retained
resource. The receiver can then acquire its own rooted wrapper in its context.

`ready()` and `wait_for(0ms)` allow nonblocking polling. Waiting on an unfinished
task from a VM-state callback or any executor worker throws, including waits on
another pool. This prevents workers from consuming their own capacity while
waiting for queued work and avoids pool-to-pool wait cycles. A ready result may
be retrieved there without waiting. Application code remains responsible for
Java locks, thread affinity, and its own synchronization; no ThreadLocal state
is copied to a worker implicitly.

Call `close()` from an ordinary native thread to stop admission, drain accepted
tasks, join all workers, and close their contexts. `close(false)` cancels queued
tasks and requests cooperative cancellation of running ones before joining.
Neither operation has a forced timeout: a body that ignores cancellation or a
blocked Java call can delay joining. Calls from VM-state callbacks and executor
workers are rejected before changing shutdown state. Explicit close is
idempotent and is the quiescence guarantee.

The destructor requests cancellation. On a thread where joining is safe, it
joins; in a callback or worker it detaches the remaining threads, which retain
their shared queue state and close themselves after finishing. Such destruction
does **not** establish quiescence. Keep the injecting DLL resident until workers
have stopped, and follow the existing root keeper's JVM-lifetime residence
contract. This API does not authorize `FreeLibrary` during Java work or provide
a general DLL-unload protocol.

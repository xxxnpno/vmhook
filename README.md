# VMHook

VMHook lets a C++ DLL work with a **running Windows x64 HotSpot JVM**: call
Java methods, create objects, access fields and collections, and install ASM
hooks. It uses private HotSpot operations without calling JNI or JVMTI APIs.
Start the JVM normally; VMHook requires no agent or JVM launch arguments.

Java calls run immediately on the DLL's own OS thread inside a
`vmhook::native_thread_context`. The context registers that same thread with
HotSpot and gives its Java objects GC-scanned roots.

| Interface | Language | Compiler |
| --- | --- | --- |
| `<vmhook/vmhook.hpp>` | C++23 permanently, without modules | Windows MinGW-w64 G++ 14+ |
| `import vmhook;` | C++26/latest with static reflection | Windows G++ 16 with the required module/reflection features |

Both interfaces share one implementation. The shipped private ABI profiles span
**Java 8–27**, including Temurin 27+35. Compatibility identifies
an exact `jvm.dll` build and collector: a Java version alone does not guarantee
support for every vendor rebuild. See [runtime diagnostics](docs/runtime-diagnostics.md)
and the [image manifest](data/native_profiles.json).

## A small wrapper

For a loaded Java class `example.Counter` with an `int value`, a constructor
`Counter(int)`, an instance method `add(int)` and a static field `total`:

```cpp
class counter : public vmhook::object<counter>
{
public:
    using vmhook::object<counter>::object;

    auto value() const -> std::int32_t
    { return get_field("value")->get(); }

    auto value(std::int32_t next) -> void
    { get_field("value")->set(next); }

    auto add(std::int32_t amount) -> std::int32_t
    { return get_method("add")->call(amount); }

    static auto get_total() -> std::int32_t
    { return static_members::get_field("total")->get(); }
};
```

The CRTP base knows `counter` and its Java binding. Instance lookups need no
`this` argument. Static wrapper methods use the inherited `static_members`
helper and are called normally as `counter::get_total()`. Typed arguments select
compatible method overloads; add an explicit descriptor, such as
`get_method("add", "(I)I")`, when you want one exact Java signature.
C++ identifiers use snake_case; Java
names in strings keep their original spelling.

Run the wrapper on a DLL worker, outside `DllMain`:

```cpp
vmhook::native_thread_context context{"VMHook-worker"};
if (!context) { throw vmhook::exception{context.error()}; }
if (!vmhook::register_class<counter>("example/Counter"))
{ throw vmhook::exception{"example.Counter is not loaded"}; }

auto instance = vmhook::make_unique<counter>(std::int32_t{10});
const auto next = instance->add(5);
const auto total = counter::get_total();
```

`make_unique` uses the VM allocator and runs the real Java constructor,
including required class initialization. `make_unique_with_descriptor` selects
an overloaded constructor explicitly. Ordinary Java calls retain normal JIT
execution. Between operations the worker stays in native state, allowing GC
and safepoints to progress. Construct and close the context on its owning OS
thread; keep the DLL loaded while VMHook resources exist.

The module can supply a class name through reflection:

```cpp
class [[= vmhook::java_class("example/Counter")]] counter
    : public vmhook::object<counter>
{
public:
    using vmhook::object<counter>::object;
    // Add the wrapper members above.
};

const bool registered = vmhook::register_class<counter>();
```

## Objects, nulls and errors

Java object fields and method returns produce `std::unique_ptr<wrapper>`.
Setters, calls, constructors and hooks accept those owners, usually by const
reference. Add these members inside an appropriate wrapper:

```cpp
auto get_player() const -> std::unique_ptr<player>
{ return get_field("player")->get(); }

auto set_player(const std::unique_ptr<player>& next) -> void
{ get_field("player")->set(next); }

auto select_player(const std::unique_ptr<player>& next)
    -> std::unique_ptr<player>
{ return get_method("selectPlayer", "(Lexample/Player;)Lexample/Player;")->call(next); }
```

A Java null has an allocated C++ wrapper with `get_instance() == nullptr`.
An empty C++ owner is refused as an object input. C++ allocation failure raises
`std::bad_alloc`. Failed Java calls, invalid conversions and expired or foreign
roots raise `vmhook::exception`, including for primitive and void results.
`call`, `get` and `set` check errors immediately. Their `try_call`, `try_get`
and `try_set` counterparts allow explicit inspection; a retained Java Throwable
is available from `get_throwable()` in its live originating context.

`std::string` converts strict UTF-8; `std::u16string` and `new_string`'s UTF-16
form preserve Java code units, including isolated surrogates. Invalid UTF-8 is
refused. Java null, empty text and failed conversion remain distinct.

Owning pointers own C++ wrappers. Their Java roots belong to the current
context, follow moving GC and expire when it closes. Transfer an object between
contexts explicitly with `vmhook::retain(owner)` and `retained.acquire()`.
A registered wrapper can bind an exact defining loader using an already-rooted
anchor. Existing proxies retain their binding across re-registration. See
[retention and class loaders](docs/lifetime.md).

For independent asynchronous work, [native_executor](docs/native-executor.md)
provides bounded queues and persistent native workers. Tasks transfer copied
C++ values or explicit retained resources; they support queued deadlines,
cancellation and joined closure. They execute without waiting for a Java hook.

Metadata-bound reference field writes execute the VM's collector-aware Java
Unsafe methods. Volatile primitive/reference access and supported atomic
compare/exchange follow VM semantics; unsupported final writes are refused.
Raw reference-memory writes retain their stricter collector gate.

`monitor_lock` and `with_monitor` synchronize on the actual Java object's
monitor, interoperating with `synchronized`. Their stack-owned guard keeps the
native context alive through acquisition and release. See [Java monitors](docs/monitors.md).

## Collections, arrays and standard Java types

A declared return type supplies collection element types:

```cpp
auto get_player_entities() const -> std::vector<std::unique_ptr<player>>
{ return get_field("playerEntities")->get().to_vector(); }

auto get_relationships() const
    -> std::vector<std::pair<std::unique_ptr<player>, std::unique_ptr<player>>>
{ return get_field("relationships")->get().to_entries(); }
```

The built-in wrappers cover collections, lists, sets, maps, queues, deques and
common concrete implementations. Their CRUD operations execute ordinary Java
methods. Conversions use Java's `toArray()` and `entrySet()` and preserve its
encounter order and concurrent-mutation semantics. Every Java-null element has
an allocated wrapper; a Java-null source converts to an empty vector.

With an `auto` destination, use `to_vector<player>()` to obtain the vector
immediately. Without an explicit type, `auto` keeps a conversion object that
must be converted on its owning thread before its context closes.

`vmhook::array<T>` supports all eight primitive types, object elements and
nested arrays, with checked indexed/bulk access. `make_array<T>(length)` uses
ordinary VM allocation and exact bound component classes. Standard wrappers
include Java strings, class objects, class loaders, Throwables, enums, boxed
primitives, Optional and copied heap/direct ByteBuffer access.
The optional [class/JAR generator](docs/wrapper_generator.md) creates short
wrappers for either interface with exact descriptors and owning object types.
See [arrays and standard wrappers](docs/containers.md) for complete examples.

## Persistent hooks and observation

Register a method hook on a native worker:

```cpp
const auto id = vmhook::add_hook<counter>("add", "(I)I",
    [](vmhook::return_value& result, const std::unique_ptr<counter>& self,
       std::int32_t amount)
    {
        if (amount < 0) { result.set(self->value()); }
    });

// Later:
vmhook::remove_hook(id);
```

Hooks are persistent and explicitly removed; there are no scoped hooks.
Multiple observers share one physical stub with defined priority/order and
recursion policy. `set`, `cancel` and `set_arg` control the return and arguments.
Installation drains compilation, performs a real VM safepoint deoptimization
and prevents target compilation/inlining while hooked. It covers already
compiled and inlined callers and needs no JVM launch flags. Additional observers
reuse the policy only after verifying its completed invalidation and live exclusions
at a safepoint. [JIT policy and source audit](docs/selective-deoptimization.md)
explain why a first installation still needs global deoptimization.

`hook_field` uses ASM stubs before interpreter `getstatic`/`putstatic` primitive
and reference accesses, with replacement and prevention. Object callbacks use
owning wrappers and VM-scanned transport through nested GC. It uses no polling.
See [field hooks](docs/field_hooks.md) for access semantics and covered routes.
`on_class_loaded` reports successful class definitions. Use copied class records,
bounded event queues and `hook_when_loaded` for deferred native-side installation;
class callbacks run in VM state and must stay short.

The optional [interface callback bridge](docs/interface_callbacks.md) combines
an ordinarily loaded Java `InvocationHandler`, JDK proxies and method hooks.
Java interface calls invoke a rooted C++ callback synchronously, with explicit
closure and normal Java result/exception semantics.

`observe_method_exits` records committed normal returns into a bounded queue
after monitor and return-poll processing. Native consumers receive primitive
bits or a reference-result kind, with entry-forced returns identified separately.
See [method returns and exception flow](docs/advanced-events.md) for the exact
sites, resource bounds and remaining exception-event work.

Method entry is distinct from original method exit, and Throwable stack-trace
construction is distinct from a throw/catch event. The [hook guide](docs/hooks.md)
defines exact routes, JIT cost, borrowed callback lifetime and resource reclamation.
Removal does not authorize unloading the DLL.

`runtime_capabilities()` provides a passive preflight. `observe_runtime`,
`observe_java_threads`, `observe_memory_pools` and `observe_thread_allocations`
copy management counters and platform-thread Java stack frames into ordinary C++ data.
`observe_deadlocks()` reports monitor/synchronizer cycles with copied wait-owner
information and explicit result limits.
[Observation contracts](docs/observations.md) distinguish sequential snapshots
from allocation/GC events and mixed-stack CPU profiling.

`get_instances<wrapper>()` and `for_each_instance<wrapper>()` use the verified
G1/Parallel heap iterator at a safepoint. Selected objects are rooted before
Java resumes; visitors run afterwards in the native context. The snapshot
contains allocated objects, including objects not yet reclaimed, and does not
establish a reachability graph. Unsupported profiles or collectors are refused.
See [heap snapshots and GC barriers](docs/heap-memory.md) for the locking and
root-publication protocol.

`snapshot_references(object)` copies a bounded graph of strong instance-field and
array references, with IDs for cycles and shared objects. Explicit budgets report
truncation; weak-reference internals are excluded before reading. The result
contains copied records and does not retain discovered objects. See
[reference graphs](docs/reference-graphs.md) for the traversal boundaries.

`get_loaded_classes()` takes a bounded safepoint snapshot of rooted `Class`
wrappers and copied names. Different loaders defining the same name remain
distinct. See [loaded classes](docs/class-inventory.md) for lifetime and bounds.

## Build and install

Header use:

```cpp
#include <vmhook/vmhook.hpp>
```

```sh
g++ -std=c++23 -shared -I include payload.cpp -o payload.dll
```

Installed CMake consumer:

```cmake
find_package(vmhook CONFIG REQUIRED COMPONENTS header)
target_link_libraries(payload PRIVATE vmhook::vmhook)
```

For C++26 use `COMPONENTS module`, link `vmhook::module`, then `import std;`
and `import vmhook;`. The installed package rebuilds module sources with the
consumer's compiler/configuration. It requires CMake 4.0+, Python 3.10+, Ninja
and a Windows G++ toolchain providing the required reflection and std module.
Neither module interface contains preprocessor directives.

The [packaging guide](docs/packaging.md) documents source packages, independent
consumers and the narrowly matched GNU standard-library linkage overlay created
in the build directory. Compiler installations remain untouched. The canonical
module generates the permanent C++23 header with `tools/make_header.py`.

The [four compiled DLL examples](examples/README.md) provide complete starting
points. [Benchmarks](docs/benchmarks.md) record executable workloads and measurement
limits. The [pinned H2 workload](docs/h2-application.md) exercises real JDBC
transactions over TCP, entirely from the injected C++ native thread.
[CONTRIBUTING.md](CONTRIBUTING.md) describes required validation.

## Compatibility and license

The [Windows CI](.github/workflows/build.yml) exercises both interfaces against
the pinned Java 8–27 images with normal JVM launch arguments. Java 8 uses Parallel
GC; later images use G1. External matching-PDB native support has separate gates;
hooks, durable roots and heap iteration require their audited built-in profiles.
Virtual-thread transitions, unverified relocating collectors and arbitrary JVM
shutdown races are outside the verified scope. Native Windows and Wine results
are recorded separately; finite tests do not prove every vendor/collector pair.
See [native-thread details](docs/native-threads.md) and [CHANGELOG.md](CHANGELOG.md).

VMHook retains [PolyForm Noncommercial 1.0.0](LICENSE.txt). Its full terms also
ship in installed/source packages; general commercial use is not granted.

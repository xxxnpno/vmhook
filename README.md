# VMHook

VMHook is a C++ library for an injected DLL in a **running Windows x64 HotSpot
JVM**. Write a small C++ wrapper resembling a Java class, then access its fields,
call its methods, create objects, or hook methods and static field accesses.
VMHook calls no JNI or JVMTI API and requires no JVM launch arguments.

Java calls and construction can run immediately on the DLL's own OS thread.
Create a `vmhook::native_thread_context`, then use the same wrapper API. No Java
hook or dispatch queue is needed for this route.

| Interface | Language | Compiler |
| --- | --- | --- |
| `include/vmhook/vmhook.hpp` | C++23 permanently, without modules | MinGW-w64 G++ 14+ |
| `modules/vmhook.ixx` | C++26/latest, named module and static reflection | Windows G++ with the required GCC 16 features |

Both interfaces target **Windows x64 only** and share the same implementation.
The supported Java range is **8–27**. Private HotSpot ABIs differ between builds:
native contexts require an exact built-in image profile or a matching full PDB
that passes the symbol and layout checks. Method hooks, field hooks and class
notifications currently require a built-in profile. OpenJ9 and arbitrary vendor
builds are not covered by a Java-version number alone. See
[native threads and compatibility](docs/native-threads.md).

## Wrap a Java class

Suppose the application has loaded this class:

```java
package example;

public class Counter {
    public int value;
    public static int total;

    public Counter(int initial) { value = initial; }
    public int add(int amount) { return value += amount; }
    public static int count() { return total; }
}
```

Expose only the members you need. Instance members use `get_method` and
`get_field` directly; static members use the same names through `static_members`:

```cpp
class counter : public vmhook::object<counter>
{
public:
    using vmhook::object<counter>::object;

    auto value() const -> std::int32_t
    {
        return get_field("value")->get();
    }

    auto value(std::int32_t next) -> void
    {
        get_field("value")->set(next);
    }

    auto add(std::int32_t amount) -> std::int32_t
    {
        return get_method("add", "(I)I")->call(amount);
    }

    static auto count() -> std::int32_t
    {
        return static_members::get_method("count")->call();
    }

    static auto get_total() -> std::int32_t
    {
        return static_members::get_field("total")->get();
    }
};
```

The CRTP base `object<counter>` knows the wrapper type and its registered Java
class. Its inherited instance functions also have the current wrapper as their
receiver, so neither lookup needs `this` or `*this` as an argument. Static
functions have no instance receiver; `static_members` selects class-bound
lookup explicitly. For an overloaded method, provide its JVM descriptor:
`get_method("add", "(I)I")` or `static_members::get_method("count", "()I")`.
C++ identifiers use snake_case; Java class and member names remain unchanged.

Call the static wrapper as `counter::get_total()`. `static_members` is an
inherited lookup helper, not the wrapper's name or another object to construct.
CRTP supplies the class identity; an instance function additionally supplies
its receiver. C++ cannot safely infer that receiver from a static function, so
the helper qualifier distinguishes the two inside the wrapper.

## Object fields and arguments

Use `std::unique_ptr<player>` for a Java Player reference. Object getters and
method returns allocate a C++ wrapper; setters and object arguments take the
owning pointer, usually by const reference:

```cpp
class player : public vmhook::object<player>
{
public:
    using vmhook::object<player>::object;

    auto id() const -> std::int32_t
    {
        return get_field("id")->get();
    }
};

class world : public vmhook::object<world>
{
public:
    using vmhook::object<world>::object;

    auto get_player() const -> std::unique_ptr<player>
    {
        return get_field("player")->get();
    }

    auto set_player(const std::unique_ptr<player>& next) -> void
    {
        get_field("player")->set(next);
    }

    auto select_player(const std::unique_ptr<player>& next)
        -> std::unique_ptr<player>
    {
        return get_method("selectPlayer", "(Lexample/Player;)Lexample/Player;")
            ->call(next);
    }
};
```

Register `player` and `world` with their Java class names during setup, just
as in the native-thread example below. Reference field stores currently require
Epsilon; this pointer syntax does not bypass the write-barrier restriction.
Java method calls use their normal collector-aware execution.

A Java null still has an allocated C++ wrapper. Check its instance:

```cpp
auto selected = world_instance->get_player();
if (selected->get_instance() == nullptr)
{
    // The Java player field is null.
}

// Pass a Java null with an allocated wrapper, too.
auto no_player = std::make_unique<player>(nullptr);
auto previous = world_instance->select_player(no_player);
```

An empty `std::unique_ptr<player>` is refused as an object argument. Allocation
failure throws `std::bad_alloc`; a successful object result never returns an
empty pointer. Failed calls, invalid types and expired roots report
`vmhook::exception` when converted to an owning wrapper.

## Collections and maps

Suppose `example/World.playerEntities` is a Java collection of
`example/Player` objects. Add this method to `world`:

```cpp
auto get_player_entities() const -> std::vector<std::unique_ptr<player>>
{
    return get_field("playerEntities")->get().to_vector();
}
```

The declared return type supplies `player`; there is no receiver argument or
repeated template argument. Object-array fields and Java collection method
results support the same form.

The mini wrappers include `collection`, `list`, `array_list`, `linked_list`,
`set`, `hash_set`, `linked_hash_set`, `tree_set`, `queue`, `deque`, `array_deque`,
`map`, `hash_map`, `linked_hash_map`, `tree_map` and `concurrent_hash_map`.
They provide `size()`, `is_empty()` and conversion without registering the
container's concrete implementation. Collections use Java's `toArray()`;
maps use `entrySet().toArray()`. This also supports implementations such as
unmodifiable collections and identity maps without reading their private
node layouts. Encounter order follows the Java operation.

Use a mini wrapper when you also need container operations:

```cpp
std::unique_ptr<vmhook::list> players = get_field("playerEntities")->get();
const auto count = players->size();
std::vector<std::unique_ptr<player>> copied = players->to_vector();
```

For a map, declare a vector of key/value wrapper pairs. For example, a
`Map<Player, Player>` field named `relationships` can use:

```cpp
auto get_relationships() const
    -> std::vector<std::pair<std::unique_ptr<player>, std::unique_ptr<player>>>
{
    return get_field("relationships")->get().to_vector();
}
```

`to_entries()` has the same map conversion. When using an `auto` local instead
of a declared destination type, supply the element types to materialize the
vector immediately:

```cpp
auto players = get_field("playerEntities")->get().to_vector<player>();
auto relationships = get_field("relationships")->get().to_vector<player, player>();
```

Without those template arguments, `auto` stores a deferred conversion value,
not a `std::vector`. It owns its source value, including its context-bound
root, and reads the Java collection when converted to the declared vector.
Use it on the owning thread before that context closes. Every element, key
and value has an allocated wrapper, including Java nulls: test
`entry->get_instance() == nullptr`. A null source becomes an empty vector.
Unsupported shapes, incompatible registered wrapper types, expired
roots and failed Java calls throw `vmhook::exception`. The returned unique
pointers own C++ wrappers, while Java manages the objects. Conversion follows
the Java implementation's concurrent-mutation semantics; it does not promise
an atomic snapshot of a concurrently modified container.

## Call Java from the DLL's own thread

Run this on a DLL-created worker thread, outside `DllMain`:

```cpp
auto worker() -> void
{
    vmhook::native_thread_context java{"VMHook-worker"};
    if (!java)
    {
        throw vmhook::exception{java.error()};
    }

    if (!vmhook::register_class<counter>("example/Counter"))
    {
        throw vmhook::exception{"example.Counter is not loaded"};
    }

    auto instance = vmhook::make_unique<counter>(std::int32_t{10});
    const auto value = instance->add(5);
    const auto total = counter::get_total();
    std::cout << value << ", total: " << total << '\n';
}
```

The context registers **this same OS thread** with HotSpot using private VM
operations. Calls execute synchronously on it, including normal JIT-compiled
methods. Between operations the thread remains in native state, so C++ waits
allow the JVM to reach safepoints and collect garbage. `vmhook::make_unique`
uses HotSpot's collector-aware allocator and invokes the real Java constructor;
`std::make_unique` only constructs a C++ wrapper. The Java class must already be
loaded and concrete; VMHook performs the required class initialization.
`vmhook::make_unique` throws `vmhook::exception` if Java construction fails,
and C++ wrapper allocation failure throws `std::bad_alloc`.

Construct and close the context on the same thread. Its destructor closes it;
`close()` is also available when explicit cleanup is useful. Contexts cannot be
copied, moved, or nested on one thread. A plain native thread without a valid
context is refused. Never let a worker's exception escape its OS-thread entry
function, and keep the DLL loaded while its contexts or hooks are active.

The module can put the Java class name on the wrapper at compile time:

```cpp
class [[= vmhook::java_class("example/Counter")]] counter
    : public vmhook::object<counter>
{
public:
    using vmhook::object<counter>::object;
    // The same wrapper members as above.
};

// During setup, inside the native context:
const bool registered = vmhook::register_class<counter>();
```

The C++23 interface uses explicit string registration.

## Results and object lifetime

A missing member throws `vmhook::exception` when the lookup's `->` is used;
test the lookup first when absence is expected. Keep the raw call result when
you need to distinguish a refused call or Java exception from a valid zero,
null, or void result:

```cpp
const auto result = counter::static_members::get_method("count", "()I")->call();
if (!result.was_invoked() || result.threw() || result.result_failed)
{
    // result.exception_class identifies a reported Java exception.
    throw vmhook::exception{"Counter.count failed"};
}
const auto count = static_cast<std::int32_t>(result);
```

Direct conversion to a primitive returns its default value when invocation is
refused. A reported Java exception is consumed by the native call backend so
the next call can proceed. `result_failed` reports native result bookkeeping
failure even when the Java method ran; owning object and collection conversions
refuse it instead of returning a Java-null wrapper.
Both instance lookups and `static_members` lookups
support boolean checks before using `->`.

`std::string` carries UTF-8 text for a Java String, and
`std::unique_ptr<wrapper>` represents a wrapped Java object. Object-valued
fields, method results, `self()`, `root<wrapper>::get()` and hook
receivers/arguments use this form. Pass object inputs as unique pointers or
const references to them;
do not dereference them into a by-value wrapper or wrapper-reference argument.
The unique pointer owns the **C++ wrapper**. Inside a
native context, constructed objects, object-valued results and reference field
reads carry VM-scanned roots. Those roots follow moving GC during calls and
while the worker is idle. The wrapper's shared root ticket retains that
context-bound protection; it does not become a cross-thread handle.

`new_object`, `new_array` and `new_string` also return owning pointers, with
`any_object` as the default wrapper type. `new_object` does not run a Java
constructor; use `vmhook::make_unique` for normal construction. Array allocation
retains its narrower supported callback/TLAB requirements.

A Java null produces a non-empty unique pointer with
`pointer->get_instance() == nullptr`. Empty C++ pointers are not Java nulls and
are refused as object inputs. Allocation failure throws `std::bad_alloc`.
An expired or foreign-thread rooted wrapper throws `vmhook::exception` from
`get_instance()`; it does not silently turn into a Java null.

Use each rooted wrapper on its context's owning thread. Closing the context
invalidates its rooted wrappers, object-valued results and derived references;
foreign-thread and expired uses are refused. Primitive values, UTF-8
`std::string`s and ordinary C++ values copied out remain usable. A vector's
wrapper elements still retain their context-bound lifetime. A receiver-bound
method proxy must not outlive its C++
wrapper. `get_instance()` exposes an address snapshot, not a permanently stable
pointer: do not save or dereference raw heap addresses across GC.

## Hook a method

Hooks have explicit persistent lifetime. A callback receives
`vmhook::return_value&`, followed by the receiver for an instance method and
then the Java arguments. Register and remove hooks on a DLL worker, outside
`DllMain`:

```cpp
auto add_hook(vmhook::return_value& result,
              const std::unique_ptr<counter>& self,
              std::int32_t amount) -> void
{
    if (amount < 0)
    {
        result.set(self->value());
    }
}

const bool installed = vmhook::hook<counter>("add", "(I)I", &add_hook);
```

`result.set_arg(0, value)` replaces the first Java argument, excluding the
receiver. `cancel()` skips a void method and `set(value)` forces a return.
An untouched result lets the original method run. There are no scoped hooks.

Object receivers and arguments use `std::unique_ptr<wrapper>` by value or
`const std::unique_ptr<wrapper>&`. A Java-null argument is an allocated wrapper
with a null instance. Use `result.set(pointer)` for an object return and
`result.set_arg(index, pointer)` for an object argument replacement. Empty
pointers, raw object addresses and plain wrapper values are refused.

The declaring Java class must already be loaded and linked. Hook registration
does not load, link or initialize it; a class that is only loaded is refused.

Installation handles a method that has already compiled or been inlined: it
temporarily pauses compiler dispatch, drains existing compilation tasks,
performs real HotSpot deoptimization at a VM safepoint, then publishes an ASM
interpreter stub and its callback. The target stays excluded from compilation
and inlining while hooked. Unrelated methods can compile again after the
installation finishes. No JVM launch flags are required.

Method and field callbacks run synchronously on the executing Java thread, in
native state. They can use the wrapper API to call Java or create objects;
their temporary roots survive GC until the callback returns. A forced object
return also remains visible to the collector through HotSpot's normal return
epilogue, including its safepoint polls. Platform-thread frames are verified;
virtual-thread frames remain outside the verified scope. Ordinary Java
bytecode methods are supported; native, abstract, constructor, class-initializer
methods and methods with an active HotSpot intrinsic ID are refused. See
[hooks and notifications](docs/hooks.md)
for the installation cost, callback lifetime and failure boundaries.

## Hook a static field

Register a callback before Java reads or writes a static primitive field:

```cpp
const bool installed = vmhook::hook_field<counter, std::int32_t>("total",
    [](vmhook::field_access<std::int32_t>& event)
    {
        if (event.is_write() && event.value() < 0)
        {
            event.prevent(); // Skip this write.
        }
        else if (event.is_read())
        {
            event.set(42);   // Supply this read's result without reading the field.
        }
    });

// Later:
vmhook::unhook_field<counter>("total");
```

The callback is reached through generated ASM stubs in HotSpot's `getstatic`
and `putstatic` templates. It uses no polling or debug-register watchpoint.
Previously compiled accessors are deoptimized, and methods containing static
field bytecodes remain interpreted while any field hook is active, including
methods in newly loaded classes. This also suppresses accessor intrinsics that
would bypass those bytecodes. Other methods can continue to compile. Removing
the last field hook restores VMHook's owned exclusions and accessor entries.

All eight Java primitive types are supported. Reference fields are refused.
Compile-time constants and accesses through reflection, Unsafe or native VM
code do not execute these bytecodes and are outside this hook's scope.

## Observe new classes

```cpp
static std::atomic<std::uint64_t> counter_definitions{};
const auto observer = vmhook::on_class_loaded(
    [](const vmhook::class_load_event& event) noexcept
    {
        if (event.internal_name == "example/Counter")
        {
            counter_definitions.fetch_add(1, std::memory_order_relaxed);
        }
    });

// Later:
vmhook::remove_class_observer(observer);
```

A nonzero ID means registration succeeded; ignoring it keeps the callback
installed. Notifications follow successful definitions, including bootstrap,
ByteBuffer and hidden/anonymous paths, and new or restored array classes.
Failed definitions do not notify, and already-loaded classes are not replayed.
`event.defining_loader_data` distinguishes same-name classes from different
defining loaders. The callback runs in VM state on the loading thread and must
stay short: do not block, call Java or install hooks from it. Event metadata is
borrowed for that callback only.

`vmhook::shutdown_hooks()` disables method, field, class and exception
callbacks and restores owned method compilation flags and entry patches where
possible. A callback already entered may finish afterward. Retired native
stubs and class-observer captures remain resident; keep the DLL loaded for the
JVM's lifetime.

`vmhook::dispatch` remains available for work that should execute on an existing
Java thread. That optional route needs an installed hook the application keeps
reaching; its future may otherwise remain pending. Direct native contexts do
not depend on it.

## Build

For the header:

```cpp
#include <vmhook/vmhook.hpp>
```

```sh
g++ -std=c++23 -shared -I include payload.cpp -o payload.dll
```

A consuming CMake project needs CMake 3.28+ and can link the header target:

```cmake
add_subdirectory(vmhook)
target_link_libraries(payload PRIVATE vmhook::vmhook)
```

For the module, use CMake 4.0+, Python 3.10+ and a Windows G++ toolchain providing
`import std`, C++26 reflection and named modules. Ninja is recommended:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DVMHOOK_BUILD_MODULE=ON
cmake --build build
```

```cmake
set(VMHOOK_BUILD_MODULE ON CACHE BOOL "Build the VMHook module")
add_subdirectory(vmhook)
target_link_libraries(payload PRIVATE vmhook::module)
```

```cpp
import std;
import vmhook;
```

Neither `.ixx` interface contains preprocessor directives. The Windows module
imports the SDK boundary as a header unit. Static reflection provides
compile-time class annotations, type names and enumerator names; live VM state
and private ABI validation remain runtime responsibilities.

The canonical module generates the standalone C++23 header:

```sh
python tools/make_header.py
python tools/make_header.py --check
```

`tools/build_module.py` builds the standard library, SDK header unit and named
modules in dependency order. Build module artifacts with the compiler and
flags of your application; compiled module caches are not portable releases.

For affected GCC 16 Windows builds, the tool creates a standard-library header
overlay **inside the build directory** so `import std` links correctly with
`std::filesystem` and `std::future`. It preserves the libstdc++ ABI symbol and
leaves installed compiler and SDK headers untouched. CMake supplies the overlay
include path automatically; manual consumers should use the flags printed by
the tool.

Enable repository checks with `-DBUILD_TESTING=ON`, build, then run
`ctest --test-dir build --output-on-failure`.

## Runtime boundaries and validation

Native contexts require supported collector metadata. The tested default
collectors are Parallel GC on Java 8 and G1 on later builds. ZGC, Shenandoah and
other concurrent relocating collectors are refused. Public reference field
stores require Epsilon until a write-barrier backend exists; primitive stores
use a separate path. Typed reference operations require verified compressed
oops.

Native-context and method-callback object and String allocation uses VM routines,
including on Java 27 with default compact headers. Raw array allocation has
narrower layout/allocation requirements.
Class lookup requires an already-loaded class and can be ambiguous across
class loaders. The library does not establish safety during concurrent JVM
shutdown or arbitrary unloading of cached metadata.

[Native-thread documentation](docs/native-threads.md) records the private ABI
checks, root lifetimes, reproducible runtime tests and exact build scope.
The [profile manifest](data/native_profiles.json) identifies the built-in images.
The [Windows CI workflow](.github/workflows/build.yml) builds both interfaces
and exercises the exact manifest images across Java 8–27. Direct-native tests
cover same-thread calls, real constructors, JIT execution, moving GC,
receiver binding and collection conversions. Collection regressions include
ordered and unmodifiable containers, identity/concurrent maps, nulls,
interface and subclass elements, deferred results, invalid types and expired
contexts. Object-reference checks distinguish allocated Java-null wrappers
from empty C++ owners in getters, calls, constructor arguments, collections
and hooks.

The observation fixtures verify actual compilation and inlining before hooks,
VM deoptimization, all eight primitive field types, class notifications and
nested callback calls with rooted returns during independent GC. Platform
thread frames are covered; virtual thread frames remain outside the verified
scope. Tests launch JVMs normally with their default collectors.

Each validation result identifies the tested commit and execution environment.
[PR #44](https://github.com/xxxnpno/vmhook/pull/44) records the preceding
collection implementation, before the allocated-wrapper Java-null contract.
Native Windows runs and separate Wine runs
are distinct evidence. These finite checks concern the exact images and
tested collectors, not every vendor/collector combination. See
[hook fixtures](docs/hooks.md) and the native-thread guide for reproduction.

See [CONTRIBUTING.md](CONTRIBUTING.md) for validation expectations and
[CHANGELOG.md](CHANGELOG.md) for migration notes.

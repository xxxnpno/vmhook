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

Expose only the members you need. `getMethod` and `getField` work for both static
and instance members; an instance lookup supplies its receiver:

```cpp
class Counter : public vmhook::object<Counter>
{
public:
    using vmhook::object<Counter>::object;

    auto value() const -> std::int32_t
    {
        return getField("value", *this)->get();
    }

    auto value(std::int32_t next) -> void
    {
        getField("value", *this)->set(next);
    }

    auto add(std::int32_t amount) -> std::int32_t
    {
        return getMethod("add", *this)->call(amount);
    }

    static auto count() -> std::int32_t
    {
        return getMethod("count")->call();
    }

    static auto total() -> std::int32_t
    {
        return getField("total")->get();
    }
};
```

There is no separate `staticMethod` or `staticField`. For an overloaded method,
provide its JVM descriptor: `getMethod("add", "(I)I", *this)` or
`getMethod("count", "()I")`.

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

    if (!vmhook::register_class<Counter>("example/Counter"))
    {
        throw vmhook::exception{"example.Counter is not loaded"};
    }

    auto counter = vmhook::make_unique<Counter>(std::int32_t{10});
    if (!counter || !counter->get_instance())
    {
        throw vmhook::exception{"Counter construction failed"};
    }

    const auto value = counter->add(5);
    const auto total = Counter::count();
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

Construct and close the context on the same thread. Its destructor closes it;
`close()` is also available when explicit cleanup is useful. Contexts cannot be
copied, moved, or nested on one thread. A plain native thread without a valid
context is refused. Never let a worker's exception escape its OS-thread entry
function, and keep the DLL loaded while its contexts or hooks are active.

The module can put the Java class name on the wrapper at compile time:

```cpp
class [[= vmhook::java_class("example/Counter")]] Counter
    : public vmhook::object<Counter>
{
public:
    using vmhook::object<Counter>::object;
    // The same wrapper members as above.
};

// During setup, inside the native context:
const bool registered = vmhook::register_class<Counter>();
```

The C++23 interface uses explicit string registration.

## Results and object lifetime

A missing member throws `vmhook::exception` when the lookup's `->` is used;
test the lookup first when absence is expected. Keep the raw call result when
you need to distinguish a refused call or Java exception from a valid zero,
null, or void result:

```cpp
const auto result = Counter::getMethod("count", "()I")->call();
if (!result.was_invoked() || result.threw())
{
    // result.exception_class identifies a reported Java exception.
    throw vmhook::exception{"Counter.count failed"};
}
const auto count = static_cast<std::int32_t>(result);
```

Direct conversion to a primitive returns its default value when invocation is
refused. A reported Java exception is consumed by the native call backend so
the next call can proceed. The legacy snake_case instance lookups return
optionals and remain available.

`std::string` carries UTF-8 text for a Java String, and
`std::unique_ptr<Wrapper>` represents a wrapped Java object. The unique pointer
owns the **C++ wrapper**. Inside a
native context, constructed objects, object-valued results and reference field
reads carry VM-scanned roots. Those roots follow moving GC during calls and
while the worker is idle. Copies and supported `borrowed`/`ref` conversions
retain that context-bound protection; they do not become cross-thread handles.

Use each rooted wrapper on its context's owning thread. Closing the context
invalidates its rooted wrappers, object-valued results and derived references;
foreign-thread and expired uses are refused. Ordinary C++ values already copied
out remain usable. A receiver-bound method proxy must not outlive its C++
wrapper. `get_instance()` exposes an address snapshot, not a permanently stable
pointer: do not save or dereference raw heap addresses across GC.

## Hook a method

Hooks have explicit persistent lifetime. A callback receives
`vmhook::return_value&`, followed by the receiver for an instance method and
then the Java arguments. Register and remove hooks on a DLL worker, outside
`DllMain`:

```cpp
auto add_hook(vmhook::return_value& result,
              const std::unique_ptr<Counter>& self,
              std::int32_t amount) -> void
{
    if (amount < 0)
    {
        result.set(self->value());
    }
}

const bool installed = vmhook::hook<Counter>("add", "(I)I", &add_hook);
```

`result.set_arg(0, value)` replaces the first Java argument, excluding the
receiver. `cancel()` skips a void method and `set(value)` forces a return.
An untouched result lets the original method run. There are no scoped hooks.

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
their temporary roots survive GC until the callback returns. Ordinary Java
bytecode methods are supported; native, abstract, constructor, class-initializer
methods and methods with an active HotSpot intrinsic ID are refused. See
[hooks and notifications](docs/hooks.md)
for the installation cost, callback lifetime and failure boundaries.

## Hook a static field

Register a callback before Java reads or writes a static primitive field:

```cpp
const bool installed = vmhook::hook_field<Counter, std::int32_t>("total",
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
vmhook::unhook_field<Counter>("total");
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
The current source and frozen probe DLLs passed these complete matrices under
**Wine 10.0**, with ordinary JVM launches and default collectors:

| Interface | Compiler | Direct native calls, Java 8–27 | JIT/field/class/callback fixtures, Java 8–27 |
| --- | --- | --- | --- |
| C++23 header | MinGW-w64 G++ 14.2 | 20/20 JVMs; 400/400 assertions | 80/80 cases; 940/940 assertions |
| C++26 module | Windows G++ 16.2 | 20/20 JVMs; 400/400 assertions | 80/80 cases; 940/940 assertions |

Each observation matrix runs four fixtures per Java major. Its 47 strict
assertions cover actual compilation and inlining, deoptimization, future
compilation, all eight primitive field types, new definitions and removal.
Every callback fixture additionally completes 24 nested calls and rooted
object returns while independent GC progresses. Source and DLL hashes are
checked before and after the run. The G++ 14.2 build passed **8/8 CTest suites**. The combined G++ 16.2 header/module build passed **15/15 CTest suites**.

Native Windows CI for this observation backend is **pending**. The
[workflow](.github/workflows/build.yml) runs both interfaces on all 20 profiled
JVMs; the Wine results above are identified separately.

The older direct-native implementation at
[commit cdbf2e5](https://github.com/xxxnpno/vmhook/commit/cdbf2e51461283f73c96cf4883f118f3b35ac67c)
passed two native Windows runs with **53/53 jobs each**:
[push](https://github.com/xxxnpno/vmhook/actions/runs/37995958477) and
[PR](https://github.com/xxxnpno/vmhook/actions/runs/37995966320).
Those runs precede the new JIT/field/class backend. Earlier G++ 16.2 header and
explicit-PDB matrices remain historical Wine results, detailed in the
[native-thread guide](docs/native-threads.md#reproduce-the-public-wrapper-tests).
These finite results concern the exact images and tested collectors, not every
vendor/collector combination. See [hook fixtures](docs/hooks.md) for reproduction.

See [CONTRIBUTING.md](CONTRIBUTING.md) for validation expectations and
[CHANGELOG.md](CHANGELOG.md) for migration notes.

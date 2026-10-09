# VMHook

VMHook is a C++ library for inspecting and hooking a **running Windows x64
HotSpot JVM** from an injected DLL. Write a C++ wrapper resembling the Java
class, then read its fields, call its methods, or intercept a method call.
VMHook does not use JNI or JVMTI and does not require JVM launch arguments.

Two interfaces expose the same runtime:

| Interface | Language | Compiler |
| --- | --- | --- |
| `include/vmhook/vmhook.hpp` | C++23, permanently; no modules | MinGW-w64 G++ 14+ |
| `modules/vmhook.ixx` | C++26/latest; named module and static reflection | MinGW-w64 G++ with the required GCC 16 features |

Both target **Windows x64 only**. Java 8 and newer HotSpot builds are the
compatibility target. VMHook relies on HotSpot internals, so this does not imply
support for OpenJ9, every vendor build, or every collector. Available operations
also depend on the layouts and capabilities detected in the running VM.

## Build

For the header, include it and compile your DLL as C++23:

```cpp
#include <vmhook/vmhook.hpp>
```

```sh
g++ -std=c++23 -shared -I include payload.cpp -o payload.dll
```

A CMake build requires CMake 3.28 or newer.

A consuming CMake project can use the header target:

```cmake
add_subdirectory(vmhook)
target_link_libraries(payload PRIVATE vmhook::vmhook)
```

For the module, use CMake 4.0+, Python 3.10+, and a Windows G++ toolchain that
provides `import std`, C++26 reflection, and named modules. Ninja is recommended:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DVMHOOK_BUILD_MODULE=ON
cmake --build build
```

In a consuming project, set `VMHOOK_BUILD_MODULE` before adding VMHook and link
`vmhook::module`:

```cmake
set(VMHOOK_BUILD_MODULE ON CACHE BOOL "Build the VMHook module")
add_subdirectory(vmhook)
target_link_libraries(payload PRIVATE vmhook::module)
```

```cpp
import std;
import vmhook;
```

The module uses reflection for compile-time type names, enumerator names, and
Java-class annotations. Neither `.ixx` file contains preprocessor directives.
The Windows module imports an SDK header unit from
`include/vmhook/detail/windows.hpp`. The C++23 header embeds that boundary and
is generated from the module with explicit C++23 replacements:

```sh
python tools/make_header.py
python tools/make_header.py --check
```

`tools/build_module.py` builds the standard library, SDK header unit, Windows
module, and VMHook module in dependency order. The CMake target supplies its
module mapper and linker settings to consumers.

Build module artifacts with the compiler and flags used by your application.
A compiled module interface is a compiler cache, not a portable release format.

Enable the repository checks with `-DBUILD_TESTING=ON`, build, and run
`ctest --test-dir build --output-on-failure`. These checks do not by themselves
establish compatibility with a live injected JVM.

## Wrap a Java class

Suppose the application has loaded this Java class:

```java
package example;

public class Counter {
    public int value;
    public static int total;

    public Counter(int initial) { value = initial; }
    public int add(int amount) { return value += amount; }
    public static int count() { return total; }
    public static void tick() { /* called regularly by the application */ }
}
```

Expose the members you need in C++. The same `getMethod` and `getField` names
serve static and instance access:

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

Register the wrapper once during setup, after the Java class has loaded:

```cpp
if (!vmhook::register_class<Counter>("example/Counter"))
{
    // The class is not available yet; retry setup when it has loaded.
    return;
}
```

With the module, place the name on the class at compile time:

```cpp
class [[= vmhook::java_class("example/Counter")]] Counter
    : public vmhook::object<Counter>
{
public:
    using vmhook::object<Counter>::object;
    // The same wrapper members as above.
};

const bool registered = vmhook::register_class<Counter>();
```

The C++23 header uses the explicit string registration shown first.

Static access needs no object. Instance access supplies `*this`, so the lookup
uses that object's runtime class. There is no separate `staticMethod` or
`staticField` API. When overloads need an exact JVM descriptor, use
`getMethod("add", "(I)I", *this)` or `getMethod("count", "()I")`.

A missing member throws `vmhook::exception` when the new lookup's `->` is used;
you can test the lookup before accessing it. Raw call results expose
`was_invoked()` to distinguish refusal from a real void/null/zero result, and
`threw()` plus `exception_class` for a Java exception. Converting directly to a
primitive return type hides a refused call behind its default value; retain the
raw result when this distinction matters. The legacy snake_case instance
accessors remain available, but return optional lookups that must be checked.

Primitive types use their matching C++ types, `std::string` represents a Java
String, and `std::unique_ptr<Wrapper>` represents another wrapped object. The
unique pointer owns the **C++ wrapper**, not the Java object and not a GC root.
Resolve and use Java references within the executing Java-thread operation;
copy ordinary C++ values out when work must cross threads.

A call refreshes its bound receiver and directly supplied C++ wrapper arguments
after GC during that call. A receiver-bound method proxy must not outlive its
wrapper. Those temporary VM handles end with the call; unrelated wrappers and
captured raw addresses are not retained across other Java calls.

## Call Java from a C++ worker thread

A native worker submits work with `vmhook::dispatch`. VMHook executes it on an
existing Java thread when that thread next reaches an installed hook. It does
not attach the native thread through JNI or fabricate a JavaThread.

Direct execution on the worker's own OS thread is possible after registering
that thread with HotSpot. An isolated, exact-build experiment has verified that
mechanism without calling JNI/JVMTI. It is not yet exposed by these wrappers;
see [native-thread calling research](docs/native-threads.md) for the tested
profile, GC lifetime requirements, and integration limits.

First install a hook on a method the application continues to execute through
the interpreter:

```cpp
if (!vmhook::hook<Counter>("tick", [](vmhook::return_value&) -> void {}))
{
    return;
}
```

Then a C++ worker can request a method call:

```cpp
auto pending = vmhook::dispatch([]() -> std::int32_t
{
    const auto result = Counter::getMethod("count")->call();
    if (!result.was_invoked() || result.threw())
    {
        throw vmhook::exception{"Counter.count invocation failed"};
    }
    return result;
});

if (pending.wait_for(std::chrono::seconds{1}) == std::future_status::ready)
{
    const auto count = pending.get();
}
```

The future becomes ready only when a Java thread services the queue. Submitting
without a reachable hook can leave it pending indefinitely; a timeout does not
cancel the task. Calling `dispatch` inside a validated VMHook callback runs the
task immediately. Resolve wrappers inside the task instead of capturing raw
Java object addresses from another thread, and capture ordinary C++ values by
value when the task may outlive its submitting scope.

Object construction belongs inside the same operation:

```cpp
auto pending = vmhook::dispatch([]() -> std::int32_t
{
    auto counter = vmhook::make_unique<Counter>(std::int32_t{10});
    if (!counter || !counter->get_instance())
    {
        throw vmhook::exception{"Counter construction unavailable"};
    }
    return counter->add(5);
});
```

`vmhook::make_unique` allocates a Java object and invokes its Java constructor;
`std::make_unique` only creates a C++ wrapper. The Java class must already be
loaded, initialized, and concrete. VMHook first tries the executing thread's
TLAB when its layout permits direct allocation. If that fails, it can invoke
an already-loaded Java `Unsafe.allocateInstance` method through the same Java
call backend, then invoke the selected constructor. This allocation route does
not call a JNI API or a native allocation export directly. Raw arrays and String
assembly still use the current thread's TLAB without a VM allocation fallback.

## Hook a method

A hook mirrors the Java parameters, with `vmhook::return_value&` first. An
instance method also receives its wrapper as the first parameter after that:

```cpp
auto add_hook(vmhook::return_value& result,
              const std::unique_ptr<Counter>& self,
              std::int32_t amount) -> void
{
    if (amount < 0)
    {
        result.set(std::int32_t{0}); // return immediately instead of running Java
    }
}

const bool installed = vmhook::hook<Counter>("add", "(I)I", &add_hook);
```

`result.set_arg(0, value)` replaces the first Java argument; the receiver is
not counted. `result.cancel()` skips a void method, while `result.set(value)`
forces a return value. Hooks persist until `vmhook::shutdown_hooks()`; there
are no scoped hooks.

VMHook intercepts HotSpot's shared interpreter entry stubs and dispatches by
the actual `Method*`. It leaves bytecode, JIT flags, and compiled method entries
alone. Compiled or inlined calls can bypass the hook, including calls compiled
after installation. Ordinary safepoints do not remove existing inlined code.
A successful install confirms registration of an interpreter observer; test
the call paths you need on the target JVM.

Installation suspends other process threads and checks their contexts before
patching code. Unsupported stub layouts or foreign patches are refused.
`shutdown_hooks()` removes callbacks and attempts to restore patched entries.
Retired trampolines are retained because VMHook cannot prove that every JVM
thread has left generated code. Keep the injected DLL loaded.

## Runtime boundaries

- JNI-free invocation is available in validated VMHook Java-thread callbacks
  and dispatched tasks. Direct invocation from an arbitrary native thread is
  refused.
- Raw Java object addresses can move or die during GC. Snapshot/epoch checks
  detect some stale references; they do not pin objects or prove safety against
  concurrent collection. A future is not a GC root.
- Public Java reference field stores are supported only for a detected Epsilon
  collector. Moving collectors need write-barrier support that VMHook does not
  implement. Primitive field writes have a separate path.
- Typed reference fields and object-returning method calls currently require
  verified compressed oops. Uncompressed references are available only through
  lower-level APIs with an explicitly correct slot width; typed wrappers refuse
  them.
- Compact headers are detected from VM flags and exported mark-word coordinates.
  Supported states permit class/array reads; unknown or forwarding states are
  refused. Object construction can use the Java allocation backend, while raw
  String/array assembly still needs ordinary headers and available TLAB space.
  [JEP 534](https://openjdk.org/jeps/534) makes compact headers the Java 27 default;
  its [OpenJDK implementation](https://github.com/openjdk/jdk/pull/30322) is the
  reference for that change.
- Unsupported layouts, concurrent relocating collectors, or unavailable
  allocation space can make individual operations unavailable.
  A class must already be loaded; name lookup alone can be ambiguous across
  class loaders.
- Invocation and allocation use experimental HotSpot call-frame and handle
  support. Native-handle extension storage is retained for process lifetime,
  including after its Java thread exits. Live moving-GC behavior needs separate
  validation on the exact Windows JDK build.

Compilation checks and live JVM tests establish different things. See
[CONTRIBUTING.md](CONTRIBUTING.md) for validation expectations and
[CHANGELOG.md](CHANGELOG.md) for migration notes.

## Validation evidence

Both interfaces passed the same 12 strict injected-runtime checks on these
Windows x64 Temurin builds under Wine 10.0. The C++23 DLL was built with
MinGW-w64 G++ 14.2.0; the C++26 module DLL with Windows G++ 16.2.0.
The fixtures launched their JVMs normally, without tuning or agent arguments.

| Windows JDK build | C++23 header | C++26 module |
| --- | --- | --- |
| 8u504-b01 | Passed | Passed |
| 17.0.20.1+1 | Passed | Passed |
| 21.0.12.1+1 | Passed | Passed |
| 25.0.4.1+1 | Passed | Passed |
| 27+35, default compact headers | Passed | Passed |

The checks cover metadata, native-thread refusal, hooks, mixed argument
reads/writes, worker dispatch, static/instance invocation, construction, Java
exceptions, Unicode and embedded-NUL String reads, wrapper arguments and
receivers across 32 forced-GC iterations, cancellation, and shutdown.
Seven header/source checks and six module executable suites also passed,
including reflection annotations, two translation units, memory boundaries,
codec guards, fields, registry concurrency, dispatch, patching and watchers.

These are finite results for the listed builds under Wine, not proof for every
Windows JVM or collector. The [Windows CI workflow](.github/workflows/build.yml)
repeats both interfaces and the five JDK versions on native Windows runners;
consult its run results for native-Windows evidence.

Primary implementation references:
[OpenJDK HotSpot](https://github.com/openjdk/jdk/tree/master/src/hotspot),
[Windows x64 ABI](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention),
[GCC C++ feature status](https://gcc.gnu.org/projects/cxx-status.html), and
[GCC module documentation](https://gcc.gnu.org/onlinedocs/gcc/C_002b_002b-Modules.html).

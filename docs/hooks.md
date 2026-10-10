# Hooks and class notifications

VMHook installs callbacks in a running Windows x64 HotSpot JVM without JNI,
JVMTI, bytecode replacement or JVM launch flags. The C++23 header and C++26
module expose the same APIs. Register the wrapper class before registering a
method or field hook; the [README](../README.md) shows the wrapper syntax.

The observation backend currently requires an exact built-in image from the
[Java 8–27 manifest](../data/native_profiles.json). A matching external PDB can
enable checked native-thread calls, but does not yet enable these hooks on an
unlisted image. Missing ABI facts, unsupported instruction layouts and
competing entry patches cause registration to fail.

## Method hooks

```cpp
const bool installed = vmhook::hook<Counter>("add", "(I)I",
    [](vmhook::return_value& result,
       const std::unique_ptr<Counter>& self,
       std::int32_t amount)
    {
        if (amount < 0)
        {
            result.set(self->value());
        }
    });
```

The callback starts with `return_value&`. For an instance method, the receiver
comes next; static methods omit it. Remaining parameters follow the Java
descriptor. VMHook checks that signature before installation. Pass the
descriptor when a name is overloaded. A duplicate hook on the same method is
refused.

The declaring class must already be loaded and linked. Registration does not
load, link or initialize it. A class that has merely been loaded can still lack
the interpreter entries and adapters needed for a hook; registration then
returns false.

Leave the result untouched to execute the original method. `set_arg(index,
value)` changes an argument, with index zero excluding the receiver; its boolean
result reports whether the replacement was accepted. `set(value)` skips the
body and supplies a return value, and `cancel()` skips a void method.

Hooks support ordinary Java bytecode methods with verified interpreter entries.
Native and abstract methods, `<init>`, `<clinit>` and methods with an active
HotSpot intrinsic ID are refused. An intrinsic can have a specialized entry or a
compiler expansion that bypasses its bytecode; accepting it without a separate
protocol would not establish interception.

### Compiled methods and inlined callers

Installation performs this transaction:

1. At a real VM safepoint, temporarily pause compiler dispatch and exclude the
   target from future compilation, OSR compilation and inlining.
2. Let preexisting compiler tasks finish. Short VM safepoints inspect their
   state; the installer waits outside the safepoint so compiler threads can run.
3. At another VM safepoint, invalidate existing compiled code through HotSpot's
   real deoptimization operations, activate the interpreter ASM stub and
   publish the callback.
4. Restore the previous compiler-dispatch setting. Unrelated methods and callers
   may compile again, while the hooked target remains interpreted and cannot
   be inlined.

The existing-code invalidation is deliberately conservative: it marks all
existing nmethods for deoptimization during installation. A JVM launched
without JVMTI does not necessarily record the method-evolution dependencies
needed to find every already-inlined caller. Merely clearing a target's
`Method::_code` pointer or invalidating its own compiled entry would miss those
callers. Each installation can therefore cause a temporary VM pause and
recompilation elsewhere in the application.

Compiler tasks must also drain because they can hold an older metadata
snapshot and publish code after exclusion flags change. Future compilations
see the target's exclusion policy and call the hooked interpreter entry.
Installation does not permanently disable the VM's JIT. A failed transaction
restores the temporary compiler setting and the policy it owns.

## Static field hooks

```cpp
const bool installed = vmhook::hook_field<Counter, std::int32_t>("total",
    [](vmhook::field_access<std::int32_t>& event)
    {
        if (event.is_write() && event.value() < 0)
        {
            event.prevent();
        }
        else if (event.is_read())
        {
            event.set(42);
        }
    });

const bool removed = vmhook::unhook_field<Counter>("total");
```

`hook_field` returns true when installed and keeps the registration until
`unhook_field` or `shutdown_hooks`. An instance field, mismatched descriptor,
reference field or duplicate registration is refused.

The callback runs synchronously after Java field resolution and class
initialization, but **before the heap access**. Its identity is the declaring
class and field offset, so moving a class mirror does not turn the hook into a
watch on a stale address.

| Access | Initial `value()` | Untouched event | `set(value)` | `prevent()` |
| --- | --- | --- | --- | --- |
| Read | Java zero value; the field has not been read | Perform the original read | Skip the read and return this value | Skip the read and return zero |
| Write | Value Java is about to store | Perform the original write | Store the replacement value | Skip the write |

For a read, calling both `set` and `prevent` returns the supplied value. For a
write, `prevent` takes precedence and no value is stored. A read callback does
not expose the current heap value because obtaining it would already perform
the access that the callback is allowed to prevent.

Use the C++ type matching the Java field:

| Java | C++ |
| --- | --- |
| `boolean` | `bool` |
| `byte` | `std::int8_t` |
| `char` | `std::uint16_t` |
| `short` | `std::int16_t` |
| `int` | `std::int32_t` |
| `long` | `std::int64_t` |
| `float` | `float` |
| `double` | `double` |

Generated ASM stubs intercept HotSpot's `getstatic` and `putstatic` templates;
no polling loop or hardware debug-register watchpoint observes the field.
Allowed accesses and replacement writes return through the original template,
preserving its volatile access ordering. A prevented access performs no
original volatile read or write.

While any field hook is active, VMHook excludes methods containing static field
bytecodes from compilation and inlining, even when their accesses concern an
unhooked field. It deoptimizes existing compiled code when acquiring that
policy. Successful class-definition paths apply the policy to new methods
before Java can execute or compile them. This conservative policy lets the ASM
templates observe previously compiled, inlined and newly loaded accessors;
methods without such bytecodes can continue to compile. Removing the last field
hook releases its compilation exclusions.

Accessor intrinsics need additional treatment: compiler expansions and special
interpreter entries can bypass `getstatic`/`putstatic` even when ordinary
compilation and inlining are excluded. The field policy temporarily clears
those intrinsic IDs and substitutes the VM's ordinary or synchronized
interpreter entry where an entry already exists. It leaves null entries for
HotSpot's normal class linking to populate. Removal restores the IDs, entries
and exclusion bits still owned by VMHook. This policy applies to every method
with static field bytecodes, including accessors of unhooked fields, and can
reduce their performance while a field hook is active.

Initial installation checks the loaded-class graph and concurrent definitions
again at the final safepoint before deoptimization and compiler resumption.
Only transient metadata explicitly queued by HotSpot for destruction is
excluded from that safepoint check. A still-live, preexisting hidden or anonymous
class that remains unlinked can prevent stabilization: installation returns
false after its 30-second stabilization window and restores VMHook's temporary
compiler settings and owned exclusions.

Future definition paths reserve roots and policy storage before changing method
flags; if that preparation fails, they report a prepared Java
`OutOfMemoryError` rather than exposing methods that bypass an active hook.
Class observers do not receive a successful-definition event for that failure.

This API covers **Java `getstatic`/`putstatic` execution on primitive fields**.
A compile-time constant can be embedded in bytecode without any field access.
Reflection, Unsafe and VM/native accesses do not execute these templates.
Reference-field replacement needs a collector-aware root and store protocol
and is explicitly refused. C++ `getField` operations also access fields through
their own backend and do not trigger this Java-bytecode hook.

The compatibility `watch_static_field` API uses the same pre-write ASM path.
Its callback receives zero as the first argument and the pending write value
as the second; it does not report a sampled old value. Its `watch_handle` owns
that compatibility registration. Prefer persistent `hook_field` for control
over reads and writes.

## Callback threads and roots

Method and field callbacks execute on the Java OS thread that reached the
stub. VMHook publishes the interrupted interpreter frame and performs the
Java-to-VM-to-native transition before invoking C++. C++ waits then permit
safepoints and GC. Nested wrapper calls and construction enter Java on that
same thread through the private JavaCalls backend.

The callback has a temporary rooted context. Receivers, object arguments,
constructed objects and object-valued results retain VM-scanned roots across
nested calls and moving GC. That context ends when the callback returns;
retaining a wrapper afterward or using it from another OS thread is refused.
An object forced as a method return is transferred through a VM-scanned thread
result slot before the interpreter receives its relocated address. Raw heap
address snapshots do not acquire an independent lifetime. See
[native-thread roots](native-threads.md#roots-across-calls-and-idle-gc).

Windows Java 8–11 callback returns use the private safe transition with a
memory fence, followed by the exact image's special-exit checks. This avoids
depending on a Java call-stub exception handler across the generated trampoline;
the VM's safepoint protocol remains active.

Do not install or remove hooks from a hook callback. Keep callbacks bounded
and avoid waiting for Java work that needs the interrupted thread itself.
Callback exceptions are contained by VMHook; use explicit call results to
inspect an exception thrown by a nested Java call.

Method dispatch reads an immutable published registry. It does not drop a
callback merely because an installer holds the administration mutex.

## Class notifications

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

const bool removed = vmhook::remove_class_observer(observer);
```

Registration returns a nonzero persistent ID; zero means it failed. Ignoring
the ID keeps the observer active. A callback accepting only `std::string_view`
can receive the internal class name instead.

Events follow successful ordinary and bootstrap definitions, ByteBuffer paths,
hidden/anonymous definitions, new array classes and archived array restoration.
Failed definitions do not notify. Existing classes are not replayed. VMHook
observes the VM's definition paths; it does not inject a class, alter its bytes
or allow a callback to block registration.

`class_load_event` provides the internal name, `klass`, actual defining
`defining_loader_data`, defining loader object and phase. A bootstrap loader
object is null. Same-name classes from different loaders remain distinct, and
hidden classes may have a separate defining loader-data holder. Array names
use JVM descriptors such as `[Lexample/Counter;`. Phases are
`instance_loaded`, `array_created` and `archived_array_restored`.

Unlike method and field callbacks, this notification runs in **VM state** on
the class-loading thread, potentially with VM locks held. It must be short:
do not block, call Java, request a VM operation or install hooks from
it. Copy needed data into your own preallocated buffer for later processing.
All supplied metadata, name views and heap addresses are borrowed only for
the notification. C++ callback exceptions are swallowed.

## Removal and DLL lifetime

There are no scoped method hooks. `shutdown_hooks()` disables method, field,
class and exception callbacks, releases owned compilation exclusions and
attempts to restore owned entry patches. A callback already entered can
complete after removal. Shutdown permits later registration again.

Removing a callback does **not** establish that all threads have left its ASM
stub. Retired native stubs remain allocated, and class-observer registry nodes
retain their captures even after disablement. Keep the embedding DLL loaded
for the JVM's lifetime, and do not use removal as permission to unload it.

Active method and field registrations pin their declaring classes through
VM-scanned roots. Setup privately registers a daemon, creates the rooted
`OutOfMemoryError` used by class-policy failure paths, and then parks it with
its root storage alive. It does not execute queued Java calls. User calls and
construction continue to run on their calling OS thread. This does not protect
arbitrary metadata cached elsewhere by an application, or establish safety
during concurrent JVM shutdown.

## Completed runtime validation

The current frozen probes passed the complete Java 8–27 observation matrix
under **Wine 10.0**, with no JVM tuning, agent or JIT arguments:

| Interface and compiler | Cases | Strict assertions |
| --- | ---: | ---: |
| C++23 header, MinGW-w64 G++ 14.2 | 80/80 | 940/940 |
| C++26 module, Windows G++ 16.2 | 80/80 | 940/940 |

Each Java major runs the JIT, field, class and callback fixtures. The 47 strict
assertions per major are separate from the callback fixture's 24 nested calls
and rooted object returns; every callback fixture observes independent GC
progress. Both source and probe hashes remain unchanged through their matrices.
These results cover the manifest's exact images and default collectors.
Native Windows CI for the new backend is pending; the older passing runs at
`cdbf2e5` predate it.

## Reproduce the public observation fixtures

On Windows, build both probe DLLs with runtime tests enabled:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DVMHOOK_BUILD_RUNTIME_TESTS=ON -DVMHOOK_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
python tools/run_observation_fixture.py --java path/to/bin/java.exe --javac path/to/bin/javac.exe --injector build/tests/vmhook_test_injector.exe --probe build/tests/vmhook_observation_probe.dll --callback-probe build/tests/vmhook_callback_probe.dll --evidence build/observation.json
```

For the C++26 interface, configure `-DVMHOOK_BUILD_MODULE=ON` and select
`vmhook_module_observation_probe.dll` and `vmhook_module_callback_probe.dll`.
The harness launches ordinary Java processes, injects only those subprocesses,
requires every assertion and records the probe hashes. `--mode jit`,
`--mode fields`, `--mode classes` and `--mode callbacks` select individual cases.

The JIT fixture proves the callee and caller compiled and verifies an actual
inline metadata dependency before installation. It checks deoptimization,
typed returns, void cancellation, registry contention, later caller
compilation, unrelated recompilation and restoration after shutdown. The field
fixture covers all eight primitives, ordinary and volatile fields, synchronous
replacement/cancellation, GC, a newly loaded accessor and removal. The class
fixture checks defining-loader identity, concurrent definitions, ByteBuffer,
hidden/anonymous, bootstrap and array paths, failed definitions and persistent
registration. The callback fixture checks nested Java calls and object returns
during moving GC.

The updated [Windows CI workflow](../.github/workflows/build.yml) runs these
fixtures on every exact Java 8–27 image for the G++ 14.2 header and G++ 16.2
module. A configured matrix is not a passing result; retain the commit, run
link, environment and recorded evidence when reporting a completed run.

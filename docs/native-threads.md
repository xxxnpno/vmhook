# Direct Java calls from a DLL-created thread

`vmhook::native_thread_context` lets the DLL's current Windows OS thread call
Java and create Java objects immediately. The public wrappers keep their usual
`getMethod(...)->call(...)`, `getField(...)` and `vmhook::make_unique<T>(...)`
syntax. This route does not install a hook or wait for an application method to
service a queue. See the complete [wrapper example](../README.md).

## What JNI attachment does, and what VMHook does

JNI's `AttachCurrentThread` registers the calling OS thread with the VM; it does
not send its calls to a different Java thread. HotSpot creates a JavaThread and
OSThread, records stack bounds and guard pages, installs VM TLS, prepares roots
and allocation state, publishes the thread under `Threads_lock`, and creates
its Java `Thread` object. The same physical OS thread can then enter Java using
the VM's safepoint and exception protocol.

VMHook reproduces that protocol by calling its **private constituent HotSpot
operations** directly. It never calls JNI attachment, JNI/JVMTI entry points,
function-table slots, or an attachment helper that hides such calls. HotSpot's
own lifecycle routines can notify VM observers internally; this does not make
VMHook a JVMTI client. Some internal structures retain names such as
`JNIHandleBlock`; their use is distinct from invoking a JNI API.

Registration is necessary even though the thread was created by the DLL. A
thread unknown to the VM cannot safely enter Java while GC, safepoints and
exceptions operate. After registration it is a HotSpot JavaThread backed by the
original OS thread. There is no switch to an application thread.

Calls use private `JavaCalls::call`, with the VM's handles, method metadata roots,
frame anchors and state transitions. Construction checks instantiability,
initializes the class, uses the VM's collector-aware allocator and invokes the
actual `<init>` method. String arguments use the private Java String allocator,
including UTF-16 surrogate pairs and embedded NUL characters. No JNI fallback
is used when an operation cannot run.

## Context lifetime

Create the context on a native worker outside `DllMain`, after `jvm.dll` is
loaded. Check `valid()` or its boolean conversion and inspect `error()` on
failure. The optional constructor argument sets the Java thread's name; the
default includes its Windows thread ID. VMHook registers it as a daemon thread,
so its presence does not keep the JVM alive after application threads finish.

Each wrapper operation enters VM state for the necessary work and returns to
native state afterward. Normal C++ waits and sleeps therefore let HotSpot
reach safepoints. Java method calls use normal interpreted or compiled execution;
VMHook does not disable the JIT or require JVM arguments.

Construct and close the context on the same OS thread. `close()` returns false
if called from another thread or during an active operation. The destructor
attempts closure. Contexts are neither copyable nor movable, and nesting two on
one thread is refused. Finish all operations before the worker returns; keep
the DLL loaded while a context is active. Attachment to a thread already owned
by the JVM is not this API's purpose.

Closing invalidates context-owned references before releasing VM roots and
detaching the thread. A rooted object may outlive the context as a C++ wrapper,
but it can no longer resolve or call Java. The Java object is managed by the
Java collector, not by the wrapper's C++ destructor.

## Roots across calls and idle GC

Constructed objects, object-valued call results, and reference field reads in a
native context retain real VM-scanned root slots. The VM updates these slots
when GC moves objects, including while the DLL worker is sleeping in native
state. Wrapper copies, result-to-wrapper conversions, and supported
`borrowed`/`ref` conversions share this context-bound lifetime. Field proxies
rebuild the field address from the rooted receiver before accessing it. Class
mirrors and method handles protect metadata while the operation uses it.

Temporary internal handles are released after each operation; surviving results
are promoted into the context's roots. Released root slots can be reused, so a
long-running stream of temporary method results does not require an ever-growing
live root set.

These references require their owning context and OS thread. Another context
on another thread cannot use them, even if it belongs to the same JVM. Closing
the context invalidates object-valued results, field receivers, wrapper copies
and derived references. Copy ordinary C++ values when work must cross threads;
those copied values remain usable after closure. A globally
retained raw `oop`, or a raw address passed to a new wrapper, does not acquire
this protection automatically.

`get_instance()` and lower-level `resolve()` APIs expose address snapshots.
Do not dereference or retain those addresses across a safepoint. C++ ownership,
compressed-pointer decoding and epoch checks alone do not establish a GC root.
Wrappers obtained in the older Java-hook callback route have its narrower
operation lifetime; the native-context guarantee does not extend to arbitrary
raw objects captured before the context existed.

## Java 8–27 private ABI resolution

Native attachment is selected by **exact image identity and checked ABI**, not
just the Java major version. The
[profile manifest](../data/native_profiles.json) records the Windows x64 JVM
images, publisher downloads, source revisions and symbol provenance.

Built-in profiles compare the PE image size and timestamp with its CodeView
GUID and age. Private function RVAs, types, fields, enum values and signatures
come from matching full PDBs and exported VMStructs/VMTypes. Java 9 and 10 have
exact-image profiles from a source and control-flow audit of the archived,
stripped DLLs; their audited addresses are not a runtime pattern scan.

For a compatible unlisted HotSpot build, provide its **matching full private
PDB**. VMHook searches beside the loaded JVM for `jvm.dll.pdb` and `jvm.pdb`.
Alternatively, set the process environment variable `VMHOOK_PRIVATE_PDB` to the
PDB's full Windows path before creating the first context. This can be done by
the DLL; it is not a JVM launch argument. A public-symbol-only PDB is
insufficient when it omits required private types or helpers.

The resolver checks the PDB GUID/age, Windows x64 target, required signatures,
layouts and exported VM metadata before allocating thread state. It supports
Java features 8–27. Missing or mismatched information produces an invalid
context with an error; it does not silently substitute an address from another
build. Providing a matching PDB permits checked resolution, not a promise that
every vendor's semantic changes are compatible.

The adapters cover several real ABI changes: Java 8's thread-local storage and
by-value `methodHandle`, later TLS and reference-parameter forms, stack guards
moving to a `StackOverflow` subobject, changed native-to-VM transitions, and
attachment-failure cleanup ownership. Allocation preserves the VM's required
thread alignment, including Java 8 biased-locking requirements. Thread
publication uses the VM lock protocol suitable for a not-yet-published thread.
These details are resolved from the actual image rather than guessed from a
single Java 21 layout.

Maintainers can reproduce the built-in table from the pinned publisher artifacts
and the [Java 9/10 audit facts](../data/native_audits.json):

```sh
python tools/build_private_profiles.py --cache build/private-profiles --check modules/vmhook.ixx
```

The tool verifies each DLL's SHA-256 and image identity, extracts matching PDB
metadata or checks the exact audited binary facts, and compares the generated
table with the canonical module. Downloads and intermediate profiles stay in
the build cache; PDBs and JVM binaries do not belong in the repository.

## Current runtime boundaries

The tested defaults are Parallel GC on Java 8 and G1 on later builds, with
compressed oops. ZGC, Shenandoah and other concurrent relocating collectors
are refused: their colored references and access barriers require separate
support. A matching PDB does not bypass the collector checks.

Primitive field stores use the normal primitive access path. Public **reference
field stores require Epsilon** until VMHook has the corresponding write-barrier
backend. Java methods and constructors still perform their own ordinary VM
barriers; this limitation concerns a direct C++ reference-field store.
Typed wrapper references currently require verified compressed oops.

Native-context object and String allocation uses VM allocators on both ordinary
and compact object headers. Java 27's default compact headers are part of the
runtime coverage. Raw array assembly and allocation outside a native context
have narrower TLAB/layout requirements.

Classes must already be loaded; native calls and construction perform class
initialization when required. Name-only lookup can choose the wrong copy when
multiple class loaders define the same name. Concurrent JVM shutdown, arbitrary
class unloading of externally cached metadata, other JVM implementations and
all vendor/collector combinations are outside the verified scope.

## Reproduce the public-wrapper tests

On Windows x64, configure and build with G++:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTING=ON -DVMHOOK_BUILD_RUNTIME_TESTS=ON -DVMHOOK_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
python tests/runtime/run.py --java path/to/bin/java.exe --javac path/to/bin/javac.exe --injector build/tests/vmhook_test_injector.exe --probe build/tests/vmhook_native_probe.dll --native-threads
```

For the module, also configure `-DVMHOOK_BUILD_MODULE=ON` and use
`vmhook_module_native_probe.dll`. The harness launches its own ordinary Java
process, waits until the fixture is ready, then injects the probe. It supplies
an application classpath, without tuning, agent or JIT arguments. Unsupported
operations fail the test rather than becoming a passing zero result.

The dedicated fixture checks same-thread execution, static and instance calls,
private methods and virtual/default-interface dispatch, class initialization,
real constructors and refusal of a missing constructor overload, mixed
primitive/reference arguments, Unicode and embedded-NUL Strings, Java exceptions
followed by another call, roots surviving idle GC, rejection on the wrong thread
and after closure, bounded root-slot growth, repeated and concurrent attachment,
and calls after the target method has actually compiled. It verifies that no
hook was installed.

The final public probe passed **20 strict checks on each of all 20 Java majors,
8–27**, for the C++23 header compiled with MinGW-w64 G++ 14.2, the same header
compiled with Windows G++ 16.2, and the genuine C++26 module compiled with
Windows G++ 16.2. Each compiler/interface run produced **400/400 passes** under
Wine 10.0.

Both G++ 16.2 interfaces additionally passed **360/360 checks each** on the
18 available matching full PDBs with PDB resolution explicitly selected,
covering Java 8 and 11–27. Java 9/10 passed through their exact audited image
profiles. The private backend passed its separate attachment/GC/concurrency
probe across the full range. Seven header/source checks and six module
executable suites passed; the README reflection example compiled, linked and
ran with the generated standard-library overlay.

These are finite results for the manifest's exact Windows x64 builds and default
collectors, executed under **Wine 10.0**. They do not establish native-Windows
execution or every JVM in the range.
The [Windows CI workflow](../.github/workflows/build.yml) repeats both public
interfaces across all 20 versions; consult the actual workflow results for
native-Windows evidence.

## Implementation references

The profile manifest links the source and release corresponding to each image.
Useful upstream starting points are:

- [HotSpot attachment and detachment](https://github.com/openjdk/jdk/blob/jdk-27%2B35/src/hotspot/share/prims/jni.cpp)
- [HotSpot JavaThread lifecycle](https://github.com/openjdk/jdk/blob/jdk-27%2B35/src/hotspot/share/runtime/javaThread.cpp)
- [HotSpot JavaCalls and argument handling](https://github.com/openjdk/jdk/blob/jdk-27%2B35/src/hotspot/share/runtime/javaCalls.cpp)
- [Java 8 attachment and TLS implementation](https://github.com/adoptium/jdk8u/blob/1358e1b273e2/hotspot/src/share/vm/prims/jni.cpp)
- [Windows x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention)
- [Compact object headers](https://openjdk.org/jeps/534)

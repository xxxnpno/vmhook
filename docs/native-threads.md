# Direct calls from a DLL-created OS thread

JNI does not require a hook or dispatch queue to call Java from a native worker.
`AttachCurrentThread` registers that same OS thread with the JVM. HotSpot creates
its JavaThread/OSThread state, records the stack and guards, installs TLS, prepares
GC roots and allocation state, publishes the thread under `Threads_lock`, and
creates its Java `Thread` object. Method calls and object construction then run
on the calling OS thread using the VM's normal Java execution protocol.

Those responsibilities also apply to an implementation that calls no JNI API.
The OS thread must participate in safepoints and GC root enumeration. Direct
execution can avoid an application hook and its queue, while retaining that
necessary VM registration.

## Verified isolated experiment

A separate prototype has reproduced the constituent private HotSpot attachment
operations and used `JavaCalls::call` for Java invocation. Construction uses
`InstanceKlass::check_valid_for_instantiation`, initialization, collector-aware
allocation and the actual Java constructor. No JNI/JVMTI API or function table
is called, including for attachment and detachment.

The verified runtime is **Eclipse Temurin 21.0.12.1+1, Windows x64, G1 with
compressed object and class references**. Its CodeView GUID is
`bf39a5d1-44b0-9d94-33ed-ab5d25845be7`, age 1. The matching official PDB and
exported VMStructs/VMTypes were used to check private function addresses,
layouts, enums and ABI. Every other image is refused before allocating thread
state. Java-major-version matching would be insufficient.

Actual Windows executables built with Windows G++ 16 and MinGW G++ 14 passed
under Wine 10:

- Eight attachment/detachment cycles with no residual VM TLS.
- Four additional native threads executing Java concurrently.
- Static/instance calls, class initialization, constructors and Java exceptions.
- Int/long/float/double/reference argument packing and primitive/object results.
- Descriptor rejection and refusal while an earlier Java exception is pending.
- GC during Java calls and native pauses between calls, with live rooted objects.
- 30,000 additional method calls to exercise normal JIT activity without VM flags.
- Profile rejection on the tested Java 8, 17, 25 and 27 images.

The session keeps a real HandleMark/HandleArea root scope alive, enters VM state
only for individual operations and returns to native state between them. The
VM updates its root slots during moving GC. Method metadata is protected by
the VM's methodHandle constructor/destructor. A session must end before its
thread is detached; detachment refuses active root-owning sessions.

This experiment remains separate from the public VMHook wrappers. Their native
calls continue to use the documented dispatch mechanism. The experiment does
not establish native-Windows execution, other collectors, other vendor builds,
all Java versions, arbitrary class unloading or concurrent JVM shutdown.

## Requirements for wrapper integration

An explicit native-thread context can preserve the existing `getMethod()->call`
and `make_unique` syntax while selecting the private JavaCalls backend. It must
remain in native state between operations, so C++ waits do not strand a thread
in VM state and block GC.

Objects and object-valued results need a live root ticket that survives each
call. A raw or compressed address snapshot is insufficient when the worker is
idle and GC moves the object. Method/field metadata needs class-loader lifetime
protection; field addresses must be rebuilt from their rooted receiver and
offset under a VM operation. Context closure must invalidate tickets before
freeing their roots, and reject cross-thread or expired use.

The first supported profile can therefore be explicit and narrow. Expanding
support requires verified profiles or a validated symbol/layout resolver, plus
tests on each actual target. An unverified RVA or signature scan must never
silently substitute for a missing private entry point.

## Sources

The tested JDK identifies its source as `adoptium/jdk21u@1c417fbfc2f7`.

- [Exact attachment/detachment implementation](https://github.com/adoptium/jdk21u/blob/1c417fbfc2f7/src/hotspot/share/prims/jni.cpp)
- [Exact JavaThread lifecycle](https://github.com/adoptium/jdk21u/blob/1c417fbfc2f7/src/hotspot/share/runtime/javaThread.cpp)
- [Exact JavaCalls implementation](https://github.com/adoptium/jdk21u/blob/1c417fbfc2f7/src/hotspot/share/runtime/javaCalls.cpp)
- [Exact handle and metadata-root implementation](https://github.com/adoptium/jdk21u/blob/1c417fbfc2f7/src/hotspot/share/runtime/handles.cpp)
- [Official release and matching debug image](https://github.com/adoptium/temurin21-binaries/releases/tag/jdk-21.0.12.1%2B1)

[JavaCallsWithoutJNI](https://github.com/klirii/JavaCallsWithoutJNI) demonstrates
related low-level calls on Oracle 8u191, but its initialization still invokes
JNI attachment and metadata lookup. OpenJDK's
[FFM upcall attachment helper](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/prims/upcallLinker.cpp)
also invokes JNI attachment internally. Neither satisfies a strict prohibition
on JNI invocations throughout the attachment path.

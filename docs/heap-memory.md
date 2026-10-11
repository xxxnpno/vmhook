# Heap snapshots and GC barriers

`get_instances<wrapper>()` returns rooted wrappers for objects whose exact Java
class matches the registered wrapper. `for_each_instance<wrapper>(callback)`
passes those wrappers to C++ callbacks after the JVM resumes. Both operations
require a live standalone `native_thread_context`. A snapshot observes heap
objects, not a reachability graph: a parsable object awaiting collection can
still appear. It does not report subclass instances or allocation events.

The verified backend currently covers the pinned Java 8–27 Windows x64 HotSpot
images with Java 8 Parallel GC and later G1. Other collectors, unidentified
images and external symbol substitutions are refused. Support is checked
against exact function signatures and closure layouts; it is not inferred from
reserved heap addresses.

The caller enters VM state and takes the JVM's actual `Heap_lock` with its
safepoint-aware `lock(Thread*)` operation before submitting a VMThread operation.
This is the protocol used by HotSpot's own heap-iteration caller in
[`jvmtiTagMap.cpp`](https://github.com/openjdk/jdk/blob/jdk-27%2B35/src/hotspot/share/prims/jvmtiTagMap.cpp).
Taking the lock only inside the safepoint callback would deadlock a stopped
allocation slow path. Holding it on the caller excludes allocation publication
that safepoints alone do not serialize.

At the safepoint, the collector makes TLAB tails parsable and performs its own
object walk. VMHook does not interpret dead G1 blocks using guessed object
headers: the collector's region iterator retains its mark-bitmap and
parsable-bottom checks. A first pass counts matches; a second pass fills
preallocated zeroed HandleArea slots. A bounded result can be truncated at the
requested limit. Unexpected growth beyond the reserved slots raises an error
when the requested limit has not yet been reached.

Publishing a new HandleArea slot is not itself a G1 SATB keepalive barrier.
Before publishing each captured oop, VMHook calls the collector's verified
`G1BarrierSet::enqueue_preloaded(oop)` or earlier `enqueue(oop)` function. This
function checks marking activity and uses the actual current thread's SATB
queue. Older versions use a locked shared queue for VMThread; newer versions
initialize G1ThreadLocalData for every Thread, including VMThread. The result
roots are established before Java resumes and follow object movement during
later collections. Count-only passes do not publish roots.

The matching Java 9/10 image audits record the full safepoint-aware mutex and
SATB enqueue control-flow fingerprints, the `Heap_lock` name and pointer
publication, and its caller's TLS-based tail jump. Regeneration rejects a
changed name reference, pointer store, function target or byte fingerprint.
Other pinned profiles use matching PDB identities and decorated signatures.
No JNI/JVMTI API is called by this backend.

The native runtime fixture repeatedly captures objects while Java performs
ordinary allocation and explicit GC, retains each snapshot across collection,
and invokes its objects afterward. This verifies that the captured roots and
locking protocol work in those runs. It does not by itself prove that a
concurrent marking phase occurred; SATB queue safety additionally follows the
exact source and ABI audits.

# Heap snapshots and GC barriers

`get_instances<wrapper>()` returns rooted wrappers for objects whose exact Java
class matches the registered wrapper. `for_each_instance<wrapper>(callback)`
passes those wrappers to C++ callbacks after the JVM resumes. Both operations
require a live standalone `native_thread_context`. A snapshot observes heap
objects, not a reachability graph: a parsable object awaiting collection can
still appear. It does not report subclass instances or allocation events.

The verified backend currently covers the pinned Java 8–27 Windows x64 HotSpot
images with Java 8 Parallel GC and later G1. Java 9–16 G1 additionally requires
the immutable `ClassUnloadingWithConcurrentMark` launch setting to retain its
default enabled value: those releases otherwise omit the iterator's dead-object
filter. VMHook refuses that nondefault configuration instead of returning
unmarked objects after SATB has stopped. Java 17 and later apply the iterator's
liveness checks independently of that setting. Other collectors, unidentified
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
requested limit. Once that bound is full, further collector callbacks skip
VMHook metadata reads; the collector completes its own full walk. Unexpected growth beyond the reserved slots raises an error
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

Typed arrays use their exact component class and defining loader.
`get_instances<vmhook::array<player>>()` selects that loaded `player[]` class;
a same-named component from another loader is excluded. If the valid component
binding has no array class yet, the exact snapshot is empty. The query does not
load an array class merely to search for its instances.

`is<vmhook::array<player>>()` uses Java component assignability, so it also
accepts compatible subclass arrays. The same check applies to nested arrays,
rooted refs and borrows, including empty arrays with no elements to inspect.
Java null still answers false. An unregistered, stale or foreign-context source
remains an error. `is<vmhook::any_object>()` accepts any non-null rooted Java
object without requiring an additional registry entry.

`HeapConcurrentFixture` additionally drives ordinary allocation until the
matching Java 21/27 G1 SATB active flag is observed. Its separate probe checks
that capture runs with marking active on VMThread and that 64 captured roots
remain usable after marking completes. The diagnostic flag offsets are
matching-PDB facts guarded by the full build GUID; they do not select production
addresses. This runtime witness complements the source proof that remark drains
partial VMThread SATB queues before the cycle ends.

Direct access to the bootstrap `java.lang.ref.Reference.referent` field is
refused, including reads, writes, atomic updates, raw compressed-oop echoes,
and field-hook installation. This field is a weak edge: Java 8/9 Unsafe loads
explicitly apply the G1 referent SATB barrier, and Java 10–27 use
`ON_UNKNOWN_OOP_REF` access decorators. Rooting a plain memory load does not
supply that barrier or the complete Reference processing rules. Use the
ordinary Java `Reference.get()` and `clear()` methods through `get_method`.
The guard matches the actual bootstrap declaring class and field offset;
application fields named `referent`, including fields shadowing that inherited
name, remain ordinary fields. The caller-supplied raw-address constructor
carries no declaring-field identity and remains an unsafe escape hatch.
Exact source URLs, hashes and relevant line numbers are recorded in
[`reference_field_access_audit.json`](../data/reference_field_access_audit.json).

## Field memory semantics

Ordinary primitive `get()`/`set()` accesses do not create a cross-thread
happens-before edge. Synchronize shared access with Java volatile fields, Java
monitors or the atomic operations below. A field declared `volatile` uses
HotSpot's matching Java Unsafe getter/setter, including its Java Memory Model
barriers, rather than raw C++ reads and writes.

Reference setters invoke the VM's actual Java accessor through rooted
JavaCalls. The selected implementation supplies collector write barriers and
volatile ordering when needed. Java 8 uses its existing Unsafe methods, so
VarHandle is not required. Object values are passed as
`const std::unique_ptr<wrapper>&`; Java null is an allocated wrapper whose
`get_instance()` returns null. An empty C++ owner is refused. The VMHook
library does not call JNI/JVMTI APIs, and requires no module-opening flags.

`compare_exchange(expected, desired)` provides a strong compare-and-swap for
exact Java `int`, `long` or reference fields. It returns success, uses volatile
ordering and collector barriers, and leaves the C++ `expected` argument
unchanged. Reference comparison uses Java object identity. Other primitive
CAS types and arbitrary memory-order settings are unavailable.

Final fields are readable; all setters and CAS operations refuse their
mutation. A low-level write would bypass Java final-field semantics and could
contradict an already-folded compiled constant. The library does not promise
that raw writes invalidate those compiler assumptions.

# Rooted reference graphs

`snapshot_references(object)` copies a bounded graph starting from an object you
already own through a rooted wrapper. Use a live standalone native context:

```cpp
vmhook::native_thread_context context;
auto player = game::get_player(); // std::unique_ptr<player>
const auto graph = vmhook::snapshot_references(player);
```

An allocated wrapper containing Java null returns an empty, complete graph.
An empty C++ `unique_ptr`, an unrooted raw-address wrapper or an expired context
is an error. Hook callback contexts are refused. Wrapper methods that happen
to be named `get_instance` or `get_native_root` cannot override this ownership
check.

The result contains copied classes, nodes and edges, with no Java object
addresses or handles. IDs are indices within this snapshot, and are not stable
between snapshots. Repeated references and cycles share a node ID. Each node
records its actual class and discovery depth; arrays also record their kind
and length. Class identity includes the defining loader: two same-named loaded
classes can have different class IDs. Changing a wrapper's registered class
does not change the runtime class of an object it already owns. The copied
result remains usable after the context closes or objects move or disappear.

An instance-field edge records its Java name, descriptor, declaring class and
field offset. An array edge records its element index. `to` is present only for
a represented strong edge. Java null, deliberately excluded fields and targets
omitted by a limit have distinct `reference_edge_state` values. Class names,
field names and descriptors are raw HotSpot Symbol bytes, which use modified
UTF-8. They do not inherit the standard UTF-8 conversion contract of VMHook's
Java String API.

## Scope and weak references

The traversal includes nonstatic Java-declared reference fields, inherited
fields and object-array elements. Primitive arrays are leaf nodes; their
element contents are not copied. Static fields and VM-injected fields are
outside this graph's scope, as are VM-only links such as Class metadata and
collector roots. This is a graph of the selected Java fields, not a retained
size calculation or an enumeration of all JVM roots.

Every instance reference field declared by the exact bootstrap
`java.lang.ref.Reference` class is excluded before its slot is read. This
conservative boundary covers weak, soft, phantom and final-reference referents
and their internal queue/discovery links. Excluded edges are reported as
`excluded_reference_field`; their nullness is deliberately unavailable. An
ordinary subclass field named `referent` remains eligible because its declaring
class differs. The snapshot never turns a weak referent into a strong root.
Use Java's ordinary `Reference.get()` method when that behavior is wanted.

`complete` means that every edge within this scope was inspected within the
requested budgets. The documented exclusions alone do not make it false.

## Budgets and errors

```cpp
vmhook::reference_graph_limits limits;
limits.max_nodes = 512;
limits.max_edges = 4096;
limits.max_depth = 8;
const auto graph = vmhook::snapshot_references(player, limits);
if (!graph.complete) {
    // Inspect node_limit, edge_limit, class_limit, depth_limit,
    // slot_limit and label_limit before interpreting the partial result.
}
```

Defaults are 256 nodes, 2,048 edges, 256 classes, depth 16, 65,536 inspected
slots and 262,144 copied label bytes. Slots count each declared field record,
including primitive/static records, and each visited object-array element.
Class and field labels share the byte budget. Existing represented nodes can
still be referenced at the depth limit, so a back-edge does not require a new
node. Truncation sets `complete` to false and its applicable limit flags; it
does not masquerade as Java null or a silently empty snapshot.

Hard bounds are 65,536 nodes, 262,144 edges, 8,192 classes, depth 256, 4,194,304
slots and 16,777,216 label bytes. Capacities must be nonzero; depth zero is
valid. Invalid limits, unreadable metadata and unsupported runtime/context
conditions raise `vmhook::exception`. A real C++ allocation failure propagates
the original `std::bad_alloc` exception, including a derived allocation error.

The operation pauses Java mutators at one verified VM safepoint. Traversal uses
preallocated buffers; C++ strings are materialized after the safepoint. Budgets
bound copied work and memory, not wall-clock pause duration. Calling this API
does not establish a Java Memory Model synchronization relationship for later
application code.

## Verified backend

The exact pinned Windows x64 HotSpot profiles cover Java 8 Parallel GC and
Java 9–27 G1. Other JVM images, collectors and external private-symbol
substitutions are refused. The backend reuses the audited heap-operation
admission and actual safepoint-aware `Heap_lock`; therefore Java 9–16 also
retain that backend's requirement that `ClassUnloadingWithConcurrentMark`
have its default enabled value. VMHook does not change JVM launch settings.

The caller's existing root is refreshed at the actual safepoint. Only strong
edges in the declared scope are traversed while Java mutators are stopped;
discovered objects are not published as new roots. Metadata labels and graph
records are copied before Java resumes. The only cached Class identity is the
well-known bootstrap Reference class for the audited JVM lifetime, with a
local mirror root retained for each capture. Application class identities are
never cached across captures.

Java 8–20 field records use the exported Java-field count and six-slot field
layout. Java 21–27 use the exported bounded UNSIGNED5 field stream. Injected
records are excluded in both layouts. Array geometry uses the existing exact
reference-slot width and verified array layout, including compact headers.
The graph does not guess the location of embedded InstanceKlass oop maps or
walk physical GC maps as if their special Reference edges were ordinary
fields. Exact publisher source hashes and relevant lines are recorded in
[`reference_graph_access_audit.json`](../data/reference_graph_access_audit.json).

The `reference_graph` runtime fixture checks cycles, repeated/null array
elements, primitive arrays, inherited and shadowed fields, weak/soft/phantom
exclusions, same-name loader identity, rebinding, all six budgets, genuine
allocation errors, root-resource plateaus and retries while Java allocates and
collects. Both editions run the same fixture on the pinned Java 8–27 matrix.

# Loaded classes

```cpp
vmhook::native_thread_context context;
for (const auto& record : vmhook::get_loaded_classes()) {
    std::cout << record.name << '\n';
    auto loader = record.type->loader();
}
```

`get_loaded_classes()` returns a bounded snapshot of classes accepted by
HotSpot's ordinary loaded-class traversal, including loaded array, hidden and
anonymous classes when that traversal includes them. Primitive `Class` objects
have no `Klass` and are outside this inventory. Each record has a copied UTF-8
binary class name and an allocated `std::unique_ptr<vmhook::class_object>`.
Independent definitions with the same name remain independent records. There
is no implicit class-name deduplication or global loader preference.

The snapshot's membership is taken in a VM safepoint. VMHook calls the verified
internal `ClassLoaderDataGraph` loaded-class iterator, whose loaded-state,
array-component and live-loader filters belong to the matching JVM image.
It preserves G1 keepalive semantics before publishing each selected mirror in
a caller-owned, preallocated GC root. C++ result allocation, Java calls and user
code run after that pause. No JNI or JVMTI entry point is called.

The default bound is 65,536 classes. Bounds from 1 through 1,048,576 are accepted.
A count above the bound, growth beyond the reserved capacity between counting
and capture, or an invalid loaded mirror throws; a successful snapshot is not
silently truncated. Retry with an appropriate bound if classes are being loaded
concurrently. Names are read from the rooted `Class` objects after capture, so
formatting can itself load additional JVM classes which belong to a later
snapshot.

Use a live standalone `native_thread_context`; snapshots are refused inside
hook callbacks. The `Class` owners remain rooted until released or that context
closes. A different context must not use those owners. Copied names remain
ordinary C++ data after closure; explicitly `retain(record.type)` if a class
owner must be acquired by another context.

This requires a verified Windows HotSpot image, private VM-operation and
`KlassClosure` ABI, internal CLDG traversal, and the verified G1 or Parallel
collector route. Unknown images and incomplete symbol profiles fail closed.
`runtime_capabilities().support(runtime_feature::loaded_class_inventory)`
reports the backend preflight; an active context and successful snapshot are
still required. The older raw `for_each_loaded_class()` graph observation
remains best effort and does not provide this snapshot or lifetime guarantee.

Active-context `find_class()` and anchor-defining-loader lookups use the same
verified keepalive traversal. They can reach a safepoint. Wrapper queries retain
their receiver before lookup and resolve it again afterward. The advanced raw
`is_instance_of(oop, name)` query requires the caller to already be inside a
rooted VM operation; an address copied earlier in native state is insufficient.
The raw String decoder validates the live object's exact bootstrap class without
a safepoint, so its caller's root and current VM operation remain required.

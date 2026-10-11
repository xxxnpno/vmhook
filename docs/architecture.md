# Runtime structure and generation

VMHook exposes one standalone C++23 header and one C++26 named module. Runtime
changes belong in `modules/vmhook.ixx`, the canonical source; `tools/make_header.py`
derives `include/vmhook/vmhook.hpp`. Its check must pass before publication.
Module-only reflection/annotations are translated to C++23 alternatives by the
generator. Neither module uses preprocessor directives. `vmhook.windows` imports
the SDK boundary as a header unit.

The canonical file keeps explicit backend boundaries:

| Component | Responsibility |
| --- | --- |
| `hotspot` metadata | VMStructs, symbols, classes, methods and object layouts |
| `private_native` | Exact image/PDB identity, typed private calls, native thread lifecycle and VM transitions |
| `native_backend` | Context ownership, HandleArea slots, result/receiver roots and checked native Java calls |
| `callback_backend` | Borrowed interpreter frames, VM/native transitions and temporary rooted callback objects |
| `field_memory_backend` | Descriptor/modifier checks and normal Java Unsafe memory operations |
| `jit_backend` | Audited VM safepoint operations, compiler coordination and deoptimization |
| hook/event registries | Persistent observer identity, immutable readers, ASM publication and deferred reclamation |
| `persistent_hook_roots` | Daemon root keeper shared by durable objects and class metadata leases |
| heap iteration | Verified collector object walkers and preallocated snapshot roots |
| container/standard wrappers | Ordinary Java operations with owning object results |
| management observations | Copied synchronous Java counters and stack snapshots |
| native executor | Bounded independent workers and explicitly transferable task data |

Large generated profile tables are bracketed separately from implementation.
`data/native_profiles.json` pins each exact image and its provenance. The private
profile extractor and generator reproduce symbols, sizes, offsets and audited
machine facts from matching files; downloaded DLLs/PDBs and generated build
caches stay outside tracked source. Adding a Java major version without verifying
those facts does not establish ABI compatibility.

A native operation enters VM state on its own registered OS thread, creates or
resolves VM-scanned roots, and invokes private JavaCalls when needed. Java methods
perform normal collector barriers and JIT execution. On leaving an operation,
the worker returns to native state so the VM can stop or collect independently.
Public wrappers are C++ owners whose root tickets are bound to that context.
Durable transfer uses keeper roots and acquires a new ticket in the receiving
context. A metadata lease retains its Class mirror, rather than treating a raw
Klass address as an unload guarantee.

Hook installation is an administrator operation: resolve and retain metadata,
validate the target/template, coordinate compilation, deoptimize at a safepoint,
then publish the physical stub and immutable observer snapshot. User callbacks
execute outside registry locks. A callback cannot wait for an administrator
holding a lock while requesting a safepoint. Class callbacks only capture or mark
pending work; deferred installation runs later on the native administrator.
Retired captures can be reclaimed when readers finish; published stubs and root
keeper machinery retain their explicit process-lifetime residence contract.

The exact heap iterator takes HotSpot's safepoint-aware `Heap_lock` on the native
caller and reserves root slots before its VM operation. Its object
visitor performs no Java calls, user callbacks or C++ allocation at the safepoint;
it applies G1's verified keepalive barrier and publishes selected objects into
those slots before the mutators resume. C++
wrapper materialization and user visitors run afterwards. Collection and Java
management snapshots instead execute ordinary Java methods sequentially; their
contracts must not be confused with a stop-the-world reachability graph.

Build tooling creates compiler/configuration-specific module artifacts in the
build directory. Installed consumers rebuild from sources; CMIs are not portable
release assets. Narrowly matched GNU standard-library linkage overlays live only
in that directory and are validated by independent two-translation-unit Debug
and Release consumers. Their patches preserve the original ABI symbols and never
permit arbitrary duplicate linker definitions.

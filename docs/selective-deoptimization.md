# JIT invalidation coverage

VMHook keeps a real VM-wide invalidation for the first hook on a method. Ordinary
HotSpot compilation does not guarantee a complete record of every inlined
method, particularly a constant leaf folded into its caller. An incomplete
caller scan could leave Java executing optimized code that never reaches the
hook. Setting `Method::_code` to null would not repair such callers or active
compiled frames.

## What the implementation can reuse

Multiple observers already shared one persistent method policy. Their previous
public publication path did not revalidate that policy at a safepoint. The
current path verifies its provenance and exclusions before publishing a prepared
immutable dispatch snapshot:

- The first transaction completed compiler-task draining and actual global
  invalidation. A live reason or exclusion flag alone is insufficient: they can
  survive a failed first VM operation.
- The retained Method, ConstMethod and current linked method still agree.
- The target has no installed compiled method and remains excluded from normal
  compilation, OSR compilation and future inlining.
- The existing method reason remains live. A field-only policy does not qualify.

A completed-invalidation marker is set only after the real invalidation returns.
Re-exclusion, restoration and release clear it. If these checks fail, the
installer uses the full compiler-pause/drain/invalidation transaction. It does
not silently publish against a partially prepared policy. A failed addition
removes only a reason acquired by that transaction; a prior observer's reason
and exclusions remain owned by that earlier installation.

Valid reuse avoids another global invalidation, preserving unrelated compiled
methods and callers compiled after the target was excluded. Removing the final
observer retains the dormant policy until `shutdown_hooks()`, as before. This is
hardening of shared-policy reuse, not a general selective-deoptimization backend.
There are no new private offsets, native aliases, JVM arguments or JNI/JVMTI calls.

## Why dependency and metadata scans are insufficient

These links identify immutable upstream source revisions, rather than a moving
branch. Java 8, 11, 21 and 27 source families were examined.

| Fact | Pinned source |
| --- | --- |
| `CodeCache::mark_for_deoptimization(Method*)` tests method dependencies; it does not discover every compiled call site. | [Java 8 CodeCache](https://github.com/adoptium/jdk8u/blob/4efe36434f44bafb854e602ccbbe1b5696fce1a2/hotspot/src/share/vm/code/codeCache.cpp#L762) |
| The dependency predicate accepts only `evol_method` records. | [Java 27 nmethod](https://github.com/adoptium/jdk/blob/efe86855fd73e30932993700813258c78b12b1c7/src/hotspot/share/code/nmethod.cpp#L3080) |
| C1 records inline evolution dependencies conditionally on capabilities known during compilation. | [Java 8 C1](https://github.com/adoptium/jdk8u/blob/4efe36434f44bafb854e602ccbbe1b5696fce1a2/hotspot/src/share/vm/c1/c1_GraphBuilder.cpp#L2059) |
| C2 also guards the evolution dependency with `jvmti_can_hotswap_or_post_breakpoint()`. | [Java 27 C2 Parse](https://github.com/adoptium/jdk/blob/efe86855fd73e30932993700813258c78b12b1c7/src/hotspot/share/opto/parse1.cpp#L521) |
| `metadata_do` visits embedded references, its metadata section and the outer Method; these are not an unconditional history of all parsed inline leaves. | [Java 8 metadata traversal](https://github.com/adoptium/jdk8u/blob/4efe36434f44bafb854e602ccbbe1b5696fce1a2/hotspot/src/share/vm/code/nmethod.cpp#L2157) |
| A scope's Method is recorded when that scope is serialized. Folded scopes need not survive to this site. | [Java 27 scope serialization](https://github.com/adoptium/jdk/blob/efe86855fd73e30932993700813258c78b12b1c7/src/hotspot/share/code/debugInfoRec.cpp#L308) |
| C2 emits scopes from surviving safepoint JVMStates or optional non-safepoint notes. | [Java 27 output](https://github.com/adoptium/jdk/blob/efe86855fd73e30932993700813258c78b12b1c7/src/hotspot/share/opto/output.cpp#L1173) |
| Non-safepoint debug recording is normally disabled in product builds without compiled-method event requests. | [Java 27 recording policy](https://github.com/adoptium/jdk/blob/efe86855fd73e30932993700813258c78b12b1c7/src/hotspot/share/code/debugInfoRec.cpp#L105) |

The capability names above describe HotSpot's upstream compilation conditions;
VMHook does not invoke JVMTI to change them. Changing a condition after injection
would not supply missing records for methods compiled earlier.

The existing global protocol is calibrated for each supported DLL/PDB identity:
Java 8–13 use marked methods and the real `VM_Deoptimize::doit`; Java 14–20 use
`Deoptimization::deoptimize_all_marked`; Java 21–27 use a real
`DeoptimizationScope`, marking and committing through that scope. Compiler jobs
are drained before any of these protocols runs. No profile table is changed by
the shared-policy hardening.

A broader selective backend would need complete, durable dependency capture
covering eliminated inline leaves, all existing compilations, indirect dispatch,
OSR and future compilation. Merely adding a metadata iterator or choosing the
correct overloaded marking function does not establish that coverage. This
remaining work is not advertised as available.

## Runtime proof

[`CoalescedJitFixture.java`](../tests/runtime/CoalescedJitFixture.java) warms genuine
C2 callers on an ordinary JVM. The injected
[`coalesced_jit_probe.cpp`](../tests/runtime/coalesced_jit_probe.cpp) reads the
actual installed nmethod addresses and compilation levels at VM safepoints. It
checks:

- The first target installation invalidates existing code and observes direct,
  inlined, MethodHandle, interface and lambda callers.
- Unrelated code and a caller recompiled under the exclusion reach C2. Their
  exact code addresses survive a second observer and dormant reuse, while the
  changed Java results prove the hook still runs.
- A refused addition preserves the first observers' live policy.
- A detached incomplete policy, changed ConstMethod and field-only reason refuse
  reuse. Restoration clears the completed-invalidation marker.
- Clearing the installed policy's marker through the test seam makes a new
  **public** observer repeat actual global invalidation before publication.
- Hooking a constant-return leaf still affects its optimized caller. Removal,
  shutdown and subsequent compilation restore original results and flags.

`folded_leaf_method_metadata=present|absent` is diagnostic, not a universal
assertion: C2's surviving scope shape can change between compilations. A genuine
absence was reproduced on the Java 8 and 21 calibrated images; Java 27 retained
it in one run and omitted it in another. A single omission disproves complete
metadata-only coverage. Neither its presence nor timing alone authorizes a
selective backend.

Build with `VMHOOK_BUILD_RUNTIME_TESTS=ON` and run the owned-JVM harness:

```text
python tools/run_observation_fixture.py --mode coalesced_jit \
  --java <jdk>/bin/java.exe --javac <jdk>/bin/javac.exe \
  --injector <build>/tests/vmhook_test_injector.exe \
  --probe <build>/tests/vmhook_observation_probe.dll \
  --coalesced-jit-probe <build>/tests/vmhook_coalesced_jit_probe.dll \
  --evidence <output>/coalesced-jit.json
```

Use `vmhook_module_coalesced_jit_probe.dll` for the module edition. The report
records exact source, class, executable and DLL hashes through the common
runtime evidence helper. The fixture adds no JVM flags and does not use WhiteBox
or a Java agent. These tests apply to the exact supported images; unknown private
ABIs continue to refuse operations.

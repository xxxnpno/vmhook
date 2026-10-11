# Committed method returns and exception flow

`observe_method_exits<wrapper>(name, descriptor, queue)` records a committed
normal Java return into a bounded queue. Its ASM leaf runs after monitor cleanup,
return safepoint processing and delayed stack-overflow failure branches. It
copies primitive result bits, an observer ID, an event ID and the Windows thread
ID. Reference, array and Java `null` returns record `method_exit_result::reference`
with zero bits; they do not expose an object address or retain the returned object.

```cpp
auto returns = std::make_shared<vmhook::method_exit_queue>(256);
auto observer = vmhook::observe_method_exits<player>("score", "()I", returns);
if (!observer) { /* Installation was refused. */ }

vmhook::method_exit_record event{};
while (returns->try_pop(event))
{
    if (event.origin == vmhook::method_exit_origin::original_body)
    {
        // int results preserve their low 32 bits, including negative values.
        auto score = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(event.bits));
        consume_score(score);
    }
}
vmhook::remove_method_exit_observer(observer);
vmhook::reclaim_method_exit_observers();
```

Drain on a native owner. If processing needs Java, that owner uses its own
`native_thread_context`. The return leaf does not invoke Java, enter VM/native
state, allocate, run a user callback, acquire a registry mutex, publish a new
frame anchor or inspect a returned object. It preserves flags, volatile integer
registers and XMM registers, then replays the original instructions. Its remaining
HotSpot tail contains frame removal, optional JFR/continuation stores and the
jump to the original caller; it introduces no extra safepoint after cleanup.

Queues use bounded storage and bounded producer attempts. The global registry
allows at most 4,096 retained observers, including inactive observers awaiting
native reclamation, which also bounds each leaf snapshot scan. `method_exit_resources()`
reports admitted records, drops, active observers, resident code, snapshots and
origin storage. Saturation loses telemetry and preserves the Java method result.
Multiple observers for the same method receive separate records with the same
event ID. This stream does not make a duration around an entry callback an exit
measurement: method entry and the final return are distinct sites.

## Origin, lifetime and refusal

`entry_forced_return` identifies a return requested by an entry hook. Such a
record must not be counted as an original-body exit. Each decision is keyed by
the current frame and exact retained Method/ConstMethod. A new entry clears every
stale decision for the reused frame address, before dormant dispatch, callback
suppression or the mounted-virtual-thread refusal. This includes a genuine empty
void method whose return bytecode is BCI zero.

The origin map has 1,024 resident Windows-thread slots, each with 64 frame
entries. It deliberately uses static storage: GNU's emulated `thread_local`
access can allocate on first use. Slots are not reclaimed on thread exit. Windows
thread-ID reuse is safe because the new entry clears stale frame decisions;
poisoned slots remain conservatively suppressed on reuse. A full thread map
suppresses events for an unknown thread. Frame-list overflow poisons that slot,
so an ambiguous return cannot be mislabeled as original. The resource report
exposes used/poisoned slots, fixed storage bytes and overflow/drop counts.

Installation retains the exact declaring class and shares the existing method
JIT policy, including deoptimization of affected compiled callers. Its temporary
internal entry observer is explicitly removed after acquiring the shared policy;
no scoped user hook is created. Removing the exit observer marks it inactive. Native reclamation releases its
queue capture after dispatch readers leave; a successful reclamation call can
publish a new snapshot while older readers still retain captures. The resource
report exposes those readers, snapshots and retained observers, while the dormant physical entry policy
remains until `shutdown_hooks()`. Shutdown disables publication before releasing
method policies. Published return stubs and the injecting DLL stay resident for
the JVM's lifetime; shutdown is not an unloading guarantee.

Each calibrated Java 8–27 image must identify all six typed return codelets,
decode the selected instruction boundary, verify the complete final tail and
publish every patch at a safepoint. Ambiguous, foreign-modified or unknown
instructions refuse installation. Java 25–27 JFR stores are checked against exact
matching-PDB aggregate/member layout facts. The collector/context and target
method checks remain those of persistent method hooks; native, intrinsic and
unsupported targets are not silently approximated. Mounted virtual threads and
unknown carrier identities do not publish events.

HotSpot's [private JavaCalls helper](https://github.com/openjdk/jdk/blob/5f92fecdd23672b8b1d2ee7bb0d25b726fb4c0e0/src/hotspot/share/runtime/javaCalls.cpp#L333)
short-circuits a direct native invocation of an empty void method, without
creating a Java activation. Such a call has no entry or return event. Calling
the same empty method through an ordinary Java invoker exercises its actual
BCI-zero return and produces the expected record.

## Exception events remain a separate backend

An exceptional return does not pass the normal-return site and produces no
normal record. `return_value::throw_exception()` delivers an existing rooted Java
Throwable from an entry callback, but VMHook does not yet publish a general
`exception_exit`, throw, rethrow or catch stream. `on_throwable_stack_trace()`
observes entry to `Throwable.fillInStackTrace()`, including explicit refreshes;
overrides or disabled stack traces can bypass it. It cannot stand in for these
exception events.

| Site | What it establishes |
| --- | --- |
| Method entry | The selected invocation reached the prepared interpreter prologue. |
| Entry-forced return | A callback requested a return; the Java body may not have run. |
| Initial return bytecode | A value is ready; monitor, poll and reserved-stack processing can still fail. |
| Verified final normal-return tail | Cleanup succeeded and the original caller receives the copied result. |
| Exception-handler selection | A local handler or activation-unwind continuation was selected. |
| Exceptional activation exit | Cleanup completed and the activation actually unwound with its final Throwable. |
| Actual catch entry | The selected Java handler began executing. |

## Pinned source evidence

These links pin the reviewed source. Runtime admission additionally checks the
exact private profile and generated code; adjacent Java versions are not ABI
proof for each other.

- [Return bytecode generation](https://github.com/openjdk/jdk/blob/5f92fecdd23672b8b1d2ee7bb0d25b726fb4c0e0/src/hotspot/cpu/x86/templateTable_x86.cpp#L2237)
  performs return polling and primitive narrowing before activation removal.
- [Final activation teardown](https://github.com/openjdk/jdk/blob/5f92fecdd23672b8b1d2ee7bb0d25b726fb4c0e0/src/hotspot/cpu/x86/interp_masm_x86.cpp#L1010)
  places unwind polling, exit notifications and delayed reserved-stack failure
  before `leave()`. The final JFR operation is a byte store; the caller SP and
  return address are restored before the jump.
- [Continuation bookkeeping](https://github.com/openjdk/jdk/blob/5f92fecdd23672b8b1d2ee7bb0d25b726fb4c0e0/src/hotspot/cpu/x86/macroAssembler_x86.cpp#L2539)
  is a comparison and conditional zero store, with no VM call.
- [Java 8 teardown](https://github.com/openjdk/jdk8u/blob/77bae344e843f0e39f927b505305ce38e3d7cafc/hotspot/src/cpu/x86/vm/interp_masm_x86_64.cpp#L536)
  has an older frame/tail layout, separately admitted by its calibrated image.
- [Throw and rethrow routing](https://github.com/openjdk/jdk/blob/5f92fecdd23672b8b1d2ee7bb0d25b726fb4c0e0/src/hotspot/cpu/x86/templateInterpreterGenerator_x86.cpp#L1367)
  restores interpreter state, performs handler selection and dispatches to a
  catch or unwind continuation. The caller's `generate_return_entry_for` is a
  continuation after an invocation, not the callee's final exit.
- [Handler selection](https://github.com/openjdk/jdk/blob/5f92fecdd23672b8b1d2ee7bb0d25b726fb4c0e0/src/hotspot/share/interpreter/interpreterRuntime.cpp#L516)
  can replace the Throwable when lookup itself throws. Reguarding failure and
  monitor rules can require an unwind even when a local handler exists.
- [Unhandled activation removal](https://github.com/openjdk/jdk/blob/5f92fecdd23672b8b1d2ee7bb0d25b726fb4c0e0/src/hotspot/cpu/x86/templateInterpreterGenerator_x86.cpp#L1547)
  stores the selected exception in the VM-result root before cleanup. A monitor
  exception can still replace it, so this point is not a committed exception exit.
- [JVMTI exception-state distinction](https://github.com/openjdk/jdk/blob/5f92fecdd23672b8b1d2ee7bb0d25b726fb4c0e0/src/hotspot/share/prims/jvmtiExport.cpp#L2161)
  distinguishes propagation, unwind and catch using per-thread state. VMHook
  neither enables nor calls those JVMTI paths.

A full synchronous exit callback with an owning reference result needs a new
root-transfer and async-exception protocol. Re-entering Java after a synchronized
monitor has already been released can deliver ThreadDeath; jumping into ordinary
unwind cleanup at that point risks releasing the monitor twice. Research
prototypes established result preservation through nested moving GC at selected
normal/unwind phases on Java 8, 21 and 27, but do not establish that async protocol.
They remain outside the production API. Exception-flow promotion also needs final
Throwable identity across lookup, monitor cleanup, `finally`, interpreted/compiled
callers, native calls and async failures, with separately proved first-throw,
rethrow, catch and committed-unwind sites.

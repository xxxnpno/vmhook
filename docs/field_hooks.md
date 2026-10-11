# Synchronous static field hooks

`hook_field` installs ASM interception for Java `getstatic` and `putstatic`
bytecodes. Installation excludes accessor methods from compilation and
deoptimizes existing compiled and inlined callers before publishing the
interpreter patches. No polling loop or JVM launch argument is used.

```cpp
vmhook::hook_field<game, std::unique_ptr<player>>("currentPlayer",
    [](vmhook::field_access<std::unique_ptr<player>>& event)
    {
        if (event.is_write())
        {
            const auto& incoming = event.value();
            if (incoming->get_instance() && incoming->id() == 13)
            {
                event.prevent();
            }
        }
        else
        {
            event.set(vmhook::make_unique<player>(std::int32_t{42}));
        }
    });

vmhook::unhook_field<game>("currentPlayer");
```

The owner wrapper and custom reference value wrappers must be registered against
their loaded Java classes; built-in wrappers use their bootstrap class contracts.
Primitive fields use their matching C++ primitive type
instead of `std::unique_ptr<player>`. Duplicate hooks and incompatible field
types are refused. Reference installation compares the field's declared Java
`Class` mirror with the wrapper's exact class and defining loader; a matching
class name from another loader is insufficient. Array wrappers must also match
the declared component class and dimensions. Use the declared field type,
rather than a base-class wrapper, when installing a hook. Installation and
removal run from a native OS worker. Allocation failure propagates before a
hook is published.

An installed hook retains its declaring class and the registered class bindings
of its owner and reference value, including nested array components. Rebinding
or unregistering a wrapper afterward does not change that hook's callback
types. During its callback, normal wrapper calls and `make_unique` use those
retained bindings. A nested field callback uses its own bindings and restores
the outer callback's bindings when it returns. Outside callbacks, wrapper
operations continue to use the current registrations. Removal releases the
retained bindings after active callbacks finish.

Callbacks run synchronously on the Java thread performing the access, with
VMHook's borrowed native callback context. Normal wrapper calls, object
allocation, and nested Java calls are available there. Reference `value()` is a
`const std::unique_ptr<T>&`; `set()` accepts the same owning reference. Java null
uses an allocated wrapper whose `get_instance()` is null. Empty C++ owners are
refused.

A write event receives the pending value before the store. `set()` replaces
that value, and `prevent()` skips the store. A read event initially contains the
Java zero value: observation does not read the field. `set()` supplies the read
result; `prevent()` alone supplies zero, or Java null for a reference. An
unmodified event resumes the original access.

Reference inputs stay in the original Java expression-stack slot while nested
Java calls can collect. Replacements remain rooted through the VM-to-Java
transition and are refreshed before the ASM continuation puts them into the
Java operand stack. The original `putstatic` template performs reference
stores, including the collector's barriers and volatile ordering.

Coverage is specifically Java static field bytecodes. Compile-time constants,
reflection, Unsafe, native VM accesses, and instance field bytecodes are outside
this API's current coverage. In particular, warmed instance accesses may use
quickened or fused interpreter templates with different TOS and BCP protocols;
they are refused until all those paths are independently verified.

The next instance-field backend needs separate verified sites for three
interpreter paths, rather than extending the static patch's field filter:

| Path | Required callback and continuation protocol |
| --- | --- |
| `getfield` / `putfield` and `nofast` variants | Retain the receiver as well as the pending reference value in the bytecode's real expression-stack cells. Skip/replacement continuations must consume both operands with the correct one- or two-slot value width. |
| `fast_*getfield` / `fast_*putfield` | Spill the cached TOS value into the Java expression stack before publishing the frame anchor, and reload relocated receiver/value cells after nested Java calls. Saving an oop in the native register-save area does not make it a GC root. Resume the original reference-store template for its collector barriers. |
| `fast_aaccess_0`, `fast_iaccess_0`, `fast_faccess_0` | These fuse `aload_0` with a field read. They load the receiver from a local and temporarily advance BCP to the following `getfield`. The callback must establish the expression-stack layout expected by that BCI's oop map, then restore the fused instruction's BCP/TOS/dispatch convention. |

HotSpot rewrites cold instance accesses into those fast bytecodes and may fuse
the preceding `aload_0`. Excluding an accessor from JIT compilation does not
disable this interpreter rewriting. The current compilation-exclusion scanner
targets static-field bytecodes; the instance extension must include all these
families, existing inlined callers, and newly prepared classes. Installing only
the cold `getfield`/`putfield` patches would silently miss warmed accesses.

The source audit covers OpenJDK's x86 template and frame implementations for
[Java 8](https://github.com/openjdk/jdk8u/blob/master/hotspot/src/cpu/x86/vm/templateTable_x86_64.cpp),
[Java 21](https://github.com/openjdk/jdk21u/blob/master/src/hotspot/cpu/x86/templateTable_x86.cpp),
and [Java 27](https://github.com/openjdk/jdk/blob/master/src/hotspot/cpu/x86/templateTable_x86.cpp).
In particular `fast_accessfield`, `fast_storefield`, and `fast_xaccess` have
different TOS transitions from `getfield_or_static`; `frame::oops_interpreted_do`
uses the live BCI's `InterpreterOopMap` to scan locals and expression-stack
slots. Each added site needs runtime instruction-boundary checks and moving-GC
tests with warmed, synchronized, volatile, null, replacement, and prevented
accesses before instance hooks can be enabled.

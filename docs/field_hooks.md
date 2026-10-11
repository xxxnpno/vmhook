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

The owner wrapper and reference value wrapper must be registered against their
loaded Java classes. Primitive fields use their matching C++ primitive type
instead of `std::unique_ptr<player>`. Duplicate hooks and incompatible field
types are refused. Installation and removal run from a native OS worker.

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

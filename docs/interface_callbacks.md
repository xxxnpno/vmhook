# Interface callbacks

`make_interface_proxy<interface_type>(handler, callback)` creates an ordinary Java
`java.lang.reflect.Proxy`. Java loads the optional
[`callback_handler.java`](../java/vmhook/callback_handler.java) through the
application's normal class loader. VMHook intercepts its ordinary `invoke`
method with the existing method hook engine. There are no Java native methods
or native registration calls in this bridge.

Register the interface against its exact class loader, obtain a rooted handler
instance from application code, and create the resource in a
`native_thread_context`:

```cpp
class listener : public vmhook::object<listener>
{
public:
    using vmhook::object<listener>::object;
};

// The application already loaded its interface and the ordinary Java helper.
vmhook::register_class<listener>("example/Listener", application_anchor);
auto proxy = vmhook::make_interface_proxy<listener>(handler,
    [](const std::unique_ptr<vmhook::reflection_method>& method,
       const std::unique_ptr<vmhook::array<vmhook::any_object>>& arguments)
        -> std::unique_ptr<vmhook::any_object>
    {
        if (method->name() == "greet")
        {
            auto name = arguments->get(0);
            return vmhook::new_string("Hello " + name->get_method(
                "toString", "()Ljava/lang/String;")->call().as_string());
        }
        throw std::runtime_error{"Unexpected listener method"};
    });

auto java_listener = proxy.acquire();
// Pass java_listener to ordinary Java methods using get_method(...)->call(...).
proxy.close();
```

`application_anchor` is a rooted application object defined by the same loader
as `example.Listener`; it selects that defining loader, rather than a class
with the same name loaded elsewhere. `handler` is an allocated `unique_ptr` to
an instance of the supplied helper,
obtained through an application method or field. A different ordinary
`InvocationHandler` can expose the same `create_proxy(Class): Object` and
`new_failure(int, String): Throwable` methods. Each supplied helper instance
belongs to one proxy resource. VMHook does not load or define the helper class.

The callback receives rooted owning wrappers through constant references. A
zero-argument Java method has an allocated `arguments` wrapper whose
`get_instance()` is null. Java-null arguments and results also use allocated
wrappers with null Java instances. Returning an empty C++ `unique_ptr` is a
failure. Return an allocated Java-null owner for `void`; return ordinary Java
boxed objects for primitive interface results. The JDK performs its normal
result cast, unboxing and checked-exception rules.

`reflection_method::name()` and `declaring_class_name()` inspect the original
Java reflection object. To distinguish overloads, use `parameter_types()` and
`return_type()`; each returned class wrapper exposes its normal Java `getName`
method. Calls to `Object.equals`, `hashCode` and `toString`
use the helper's ordinary Java identity behavior, without invoking the C++
callback. Interface default methods reach the callback like other interface
methods; automatic default-body dispatch is not supplied.

A Java exception raised by a checked VMHook call inside the callback is
forwarded as the original `Throwable`. Other C++ exceptions become Java
`RuntimeException`; `std::bad_alloc` becomes Java `OutOfMemoryError`. This
mapping requires the helper and its error factory to remain callable. If error
transport itself fails, the helper's ordinary Java body raises its explicit
unavailable exception. Allocation failures before the callback adapter starts
remain contained by the method hook boundary; they cannot always retain the
same error classification.

The resource keeps both handler and proxy alive independently of its creating
context. `acquire()` creates an owning wrapper rooted in the caller's current
context. Java calls execute the callback synchronously on their calling thread;
callbacks can run concurrently, so captured C++ state must support that use.
The callback should return promptly. The existing VM profile, collector and
thread support restrictions of the hook engine still apply.

`close()` disables future user callbacks and releases durable roots after any
callback already in progress finishes. Already acquired Java proxies remain
ordinary Java objects; further interface calls through them raise the helper's
closed/unavailable exception. `valid()` reports whether this resource is open;
it does not override hook-engine shutdown or external hook removal. Closing a
resource is idempotent. The embedding DLL must remain loaded for the hook
engine's resident code lifetime.

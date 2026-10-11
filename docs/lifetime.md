# Object retention and class loaders

Ordinary object results are `std::unique_ptr<wrapper>`. Their VM roots belong to
one native or callback context. Closing that context invalidates those wrappers;
using one from another OS thread throws. A Java null remains an allocated wrapper
whose `get_instance()` is null.

Use an explicit retained resource when an object must survive its source context:

```cpp
vmhook::retained_object<player> saved;
{
    vmhook::native_thread_context source;
    auto value = vmhook::make_unique<player>(std::int32_t{42});
    saved = vmhook::retain(value);
}

// This may execute on a different DLL-created OS thread.
{
    vmhook::native_thread_context receiver;
    auto value = saved.acquire();
    saved.reset();
    value->get_method("update")->call();
}
```

`retain` requires an allocated owning wrapper and, for a non-null Java object, an
existing live VM-scanned root. It does not promote an arbitrary raw oop snapshot.
`acquire` returns a fresh context-bound wrapper; that wrapper remains rooted after
the retained resource is reset. Retaining Java null and acquiring it also returns
an allocated null wrapper. Acquiring an empty, moved-from or reset resource throws.
A retained resource may be moved between threads; concurrently changing the same
resource still needs ordinary C++ synchronization.

The persistent root keeper parks a daemon with a GC-scanned HandleArea. Calls and
construction still execute synchronously on the caller's registered OS thread.
The keeper never services Java-call requests. Its daemon and injecting DLL remain
resident for the JVM lifetime. Durable roots require the audited exact-image
backend: Java 8 with Parallel, or the later supported profiles with G1. Other
collectors and external-symbol-only builds currently refuse non-null durable
retention. JVM exit invalidates acquisition; this API does not enable DLL unload.

## Independent class definitions

An unqualified name lookup refuses two independently loaded definitions with the
same internal name. Bind each wrapper explicitly through an already-rooted object
of the intended defining loader:

```cpp
vmhook::register_class<first_player>("example/Player", first_anchor);
vmhook::register_class<second_player>("example/Player", second_anchor);
```

Each binding retains its selected `java.lang.Class` mirror on the persistent
keeper. Static methods, fields, constructor allocation and checked typed results
use that exact class identity. Instance methods continue dispatching through the
wrapped object's runtime class. Wrapper bodies need no receiver parameter:

```cpp
static auto get_total() -> std::int32_t
{
    return static_members::get_field("total")->get();
}
```

An anchor selects its object's **defining** loader, with bootstrap classes as a
fallback. This lookup does not run `loadClass`, reproduce arbitrary parent
loader delegation, or initialize a class. Null anchors and empty C++ owners are
rejected. Anchored bindings require the durable keeper backend. On other
supported native collectors, ordinary name registration resolves an unambiguous
class afresh; it does not pretend to retain metadata across unload.

`unregister_class<wrapper>()` removes the binding and releases its mirror when
its remaining users are gone. A re-registration changes future lookups. The
advanced global raw lookup preference remains separate and is not a GC root.

For frameworks using the context class loader, explicitly configure the attached
thread:

```cpp
context.set_context_class_loader(loader);
```

The argument is an owning ClassLoader-compatible wrapper; an allocated Java-null
wrapper requests the bootstrap context loader. Empty C++ owners are rejected.
Java permission checks and errors from `Thread.setContextClassLoader` are honored.
No ThreadLocal state is implicitly copied from another thread, and application
thread-affinity requirements still apply.

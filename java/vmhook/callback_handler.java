package vmhook;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

/** Optional ordinary Java bridge. Load this class through the application's
 * normal class loader before supplying an instance to VMHook. No native
 * methods, class injection or registration protocol is used. */
public final class callback_handler implements InvocationHandler {
    private boolean claimed;

    public callback_handler() {}

    /** Each bridge instance belongs to one proxy resource. */
    public synchronized Object create_proxy(Class<?> interface_type) {
        if (claimed) throw new IllegalStateException("VMHook handler already belongs to a proxy");
        if (interface_type == null || !interface_type.isInterface()) {
            throw new IllegalArgumentException("VMHook requires a Java interface");
        }
        Object result = Proxy.newProxyInstance(interface_type.getClassLoader(),
            new Class<?>[]{interface_type}, this);
        claimed = true;
        return result;
    }

    /** Called only when a C++ failure has no original Java Throwable. */
    public Throwable new_failure(int kind, String message) {
        if (kind == 1) return new OutOfMemoryError("Native C++ allocation failed");
        return new RuntimeException(message);
    }

    /** VMHook intercepts this ordinary bytecode entry. When a subscription has
     * closed, interface calls reach the explicit Java failure below. */
    @Override public Object invoke(Object proxy, Method method, Object[] arguments) throws Throwable {
        if (method.getDeclaringClass() == Object.class) {
            String name = method.getName();
            if (name.equals("equals")) return proxy == arguments[0];
            if (name.equals("hashCode")) return System.identityHashCode(proxy);
            if (name.equals("toString")) return "VMHook interface proxy " + proxy.getClass().getName();
        }
        throw new IllegalStateException("VMHook callback is closed or unavailable");
    }
}

import java.lang.management.ManagementFactory;
import java.lang.reflect.Proxy;
import java.util.concurrent.atomic.AtomicInteger;
import vmhook.callback_handler;

public class interface_fixture {
    public interface api {
        String echo(String text);
        int add(int value);
        Object nullable(Object value);
        void notify_value(Object value);
        String fail();
        String native_fail();
        String allocation_fail();
        String slow(String value);
    }
    public static volatile api current;
    public static volatile api other;
    public static volatile boolean start;
    public static volatile boolean close_now;
    public static volatile boolean closed;
    public static volatile boolean finished;
    public static volatile int valid;
    public static volatile int error_count;
    public static volatile String failure;
    public static final IllegalStateException original = new IllegalStateException("original Java callback failure");
    public static final AtomicInteger gc_epoch = new AtomicInteger();

    public static callback_handler handler() { return new callback_handler(); }
    public static void set_current(api value) { current = value; }
    public static void set_other(api value) { other = value; }
    public static void collect() { System.gc(); }
    public static Object raise() { throw original; }
    public static Integer box_int(int value) { return Integer.valueOf(value); }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        valid++;
    }
    private static void exercise() throws Exception {
        check(Proxy.isProxyClass(current.getClass()), "ordinary Proxy class");
        check(current.toString().startsWith("VMHook interface proxy "), "Object.toString");
        check(current.equals(current) && !current.equals(other), "Object.equals");
        check(current.hashCode() == System.identityHashCode(current), "Object.hashCode");
        check(current.echo("hello \u0000\ud83d\ude00").equals("native:hello \u0000\ud83d\ude00"), "String roundtrip");
        check(other.echo("hello").equals("other:hello"), "two independent handlers");
        check(current.add(37) == 42, "boxed primitive result");
        check(current.nullable(null) == null, "allocated owner representing Java null");
        Object token = new Object();
        check(current.nullable(token) == token, "argument root identity");
        current.notify_value(token);
        check(true, "void through Java-null object result");
        try { current.fail(); throw new AssertionError("Java error was hidden"); }
        catch (IllegalStateException error) { check(error == original, "original Throwable identity"); error_count++; }
        try { current.native_fail(); throw new AssertionError("C++ error was hidden"); }
        catch (RuntimeException error) { check(error.getMessage().equals("native callback failure"), "C++ diagnostic"); error_count++; }
        try { current.allocation_fail(); throw new AssertionError("C++ allocation failure was hidden"); }
        catch (OutOfMemoryError error) { check(error.getMessage().equals("Native C++ allocation failed"), "C++ OOM mapping"); error_count++; }
        Thread[] callers = new Thread[4];
        for (int i = 0; i < callers.length; i++) {
            final int index = i;
            callers[i] = new Thread(() -> {
                try {
                    for (int n = 0; n < 16; n++) {
                        String text = "thread " + index + ":" + n;
                        if (!current.echo(text).equals("native:" + text)) throw new AssertionError("concurrent result");
                    }
                } catch (Throwable error) { failure = error.toString(); }
            }, "proxy-caller-" + i);
            callers[i].start();
        }
        for (Thread caller : callers) caller.join();
        check(failure == null, "concurrent callbacks");
        final String[] slow_result = new String[1];
        Thread slow = new Thread(() -> { slow_result[0] = current.slow("in flight"); });
        slow.start();
        while (!close_now) Thread.sleep(1);
        while (!closed) Thread.sleep(1);
        slow.join();
        check("native:in flight".equals(slow_result[0]), "in-flight callback finishes during close");
        try { current.echo("closed"); throw new AssertionError("closed proxy invoked C++"); }
        catch (IllegalStateException error) { check(error.getMessage().contains("closed"), "closed proxy refusal"); }
        check(other.echo("alive").equals("other:alive"), "closing one observer preserves another");
    }

    public static void main(String[] args) throws Exception {
        callback_handler preload = new callback_handler();
        api warm = (api)preload.create_proxy(api.class);
        for (int i = 0; i < 12000; i++) {
            try { warm.echo("warm"); } catch (IllegalStateException expected) {}
        }
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        Thread gc = new Thread(() -> {
            while (!finished) { System.gc(); gc_epoch.incrementAndGet(); try { Thread.sleep(2); } catch (InterruptedException ignored) {} }
        }, "proxy-gc");
        gc.start();
        while (!start) Thread.sleep(2);
        try { exercise(); } catch (Throwable error) { failure = error.toString(); error.printStackTrace(); }
        finished = true;
        gc.join();
        System.out.println("PROXY_FINISHED valid=" + valid + " errors=" + error_count + " failure=" + failure);
        while (true) Thread.sleep(100);
    }
}

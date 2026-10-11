import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicInteger;
public final class VirtualThreadFixture {
    public static int field_value;
    private static volatile int sink;
    public static int target(int value) { return value + 1; }
    public static int field_target() { field_value = 11; return field_value; }
    public static int run_virtual() throws Exception {
        final AtomicInteger checks = new AtomicInteger();
        final Method is_virtual = Thread.class.getMethod("isVirtual");
        Thread thread = (Thread) Thread.class.getMethod("startVirtualThread", Runnable.class).invoke(null, new Runnable() {
            public void run() {
                try {
                    int result = Boolean.TRUE.equals(is_virtual.invoke(Thread.currentThread())) ? 1 : 0;
                    if (target(5) == 6) result |= 2;
                    if (field_target() == 11) result |= 4;
                    checks.set(result);
                } catch (Exception failure) { checks.set(-1); }
            }
        });
        thread.join();
        return checks.get();
    }
    public static int run_platform() { return (target(5) == 99 ? 1 : 0) | (field_target() == 77 ? 2 : 0); }
    public static int originals() { return target(5) == 6 && field_target() == 11 ? 1 : 0; }
    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 300000; ++i) { sink = target(i); sink = field_target(); }
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        for (;;) Thread.sleep(100);
    }
}

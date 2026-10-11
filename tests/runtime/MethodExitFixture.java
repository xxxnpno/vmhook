public final class MethodExitFixture {
    private static volatile long sink;
    private static int void_calls;
    private static final RuntimeException failure = new RuntimeException("original-method-exit");
    public static boolean target_bool() { return true; }
    public static byte target_byte() { return -7; }
    public static char target_char() { return '\uabcd'; }
    public static short target_short() { return -1234; }
    public static synchronized int target_int(int value) { return value + 1; }
    public static synchronized long target_long() { return 0x123456789abcdef0L; }
    public static float target_float() { return -1.25f; }
    public static double target_double() { return 3.5; }
    public static synchronized Object target_object() { return new Payload(); }
    public static Object target_null() { return null; }
    public static int[] target_array() { return new int[] { 37 }; }
    public static void target_void() { }
    public static void invoke_empty() { target_void(); }
    public static void target_counted_void() { ++void_calls; }
    public static synchronized int target_throw() { throw failure; }
    private static final class Payload { int value = 37; }
    public static int run_checks() {
        int result = 0;
        if (target_bool() && target_byte() == -7 && target_char() == '\uabcd' && target_short() == -1234) result |= 1;
        if (target_int(4) == 5 && target_long() == 0x123456789abcdef0L) result |= 2;
        if (target_float() == -1.25f && target_double() == 3.5) result |= 4;
        if (((Payload) target_object()).value == 37 && target_null() == null && target_array()[0] == 37) result |= 8;
        target_void(); target_counted_void();
        if (void_calls == 1) result |= 16;
        try { target_throw(); } catch (RuntimeException caught) { if (caught == failure) result |= 32; }
        if (!Thread.holdsLock(MethodExitFixture.class)) result |= 64;
        System.gc(); // The queue has copied bits/kinds only, with no Java roots.
        return result;
    }
    public static int run_checks_on_new_thread() throws Exception {
        final int[] result = new int[1];
        Thread worker = new Thread(new Runnable() { public void run() { result[0] = target_int(4); } });
        worker.start(); worker.join(); return result[0];
    }
    public static int virtual_calls() throws Exception {
        final int[] result = new int[1];
        Thread worker = (Thread) Thread.class.getMethod("startVirtualThread", Runnable.class).invoke(null,
            new Runnable() { public void run() {
                try { if (((Boolean) Thread.class.getMethod("isVirtual").invoke(Thread.currentThread())).booleanValue()
                    && target_int(4) == 5) result[0] = 1; }
                catch (Exception failed) { throw new RuntimeException(failed); }
            }});
        worker.join(); return result[0];
    }
    public static int stress_calls() throws Exception {
        final Thread[] threads = new Thread[8];
        final int[] totals = new int[8];
        for (int i = 0; i < threads.length; ++i) {
            final int index = i;
            threads[i] = new Thread(new Runnable() { public void run() {
                for (int j = 0; j < 1000; ++j) { if (target_int(j) == j + 1) ++totals[index]; }
            }});
            threads[i].start();
        }
        for (int i = 0; i < 10; ++i) System.gc();
        int total = 0;
        for (int i = 0; i < threads.length; ++i) { threads[i].join(); total += totals[i]; }
        return total;
    }
    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 300000; ++i) sink = target_int(i) + target_long();
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY"); System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

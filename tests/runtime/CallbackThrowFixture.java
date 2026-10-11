public final class CallbackThrowFixture {
    private static final RuntimeException expected = new RuntimeException("forwarded exact Throwable");
    private static final Object unrelated = new Object();
    private static final CallbackThrowFixture instance = new CallbackThrowFixture();
    private static int bodies;
    private static int finally_count;
    private static volatile int sink;
    public static synchronized int target_int(Throwable failure) { ++bodies; return 1; }
    public static void target_void(Throwable failure) { ++bodies; }
    public static Object target_object(Throwable failure) { ++bodies; return null; }
    public static synchronized int sync_resume(Throwable failure) {
        return Thread.holdsLock(CallbackThrowFixture.class) && failure == expected ? 7 : -1;
    }
    public static synchronized int sync_return(Throwable failure) { ++bodies; return 9; }
    public synchronized int instance_throw(Throwable failure) { ++bodies; return 10; }
    public static int invalid_object(Object value) { return 5; }
    public static int null_throw(Throwable value) { return 6; }
    public static void collect() { System.gc(); }
    public static int run_checks() {
        bodies = 0; finally_count = 0; int result = 0;
        try { target_int(expected); } catch (RuntimeException failure) { if (failure == expected) result |= 1; }
        finally { ++finally_count; }
        try { target_void(expected); } catch (RuntimeException failure) { if (failure == expected) result |= 2; }
        finally { ++finally_count; }
        try { target_object(expected); sync_resume(expected); sync_return(expected); instance.instance_throw(expected); } catch (RuntimeException failure) { if (failure == expected) result |= 4; }
        finally { ++finally_count; }
        try { instance.instance_throw(expected); } catch (RuntimeException failure) { if (failure == expected) result |= 256; }
        finally { ++finally_count; }
        if (bodies == 0 && finally_count == 4) result |= 8;
        if (sync_resume(expected) == 7) result |= 64;
        if (sync_return(expected) == 11) result |= 128;
        if (invalid_object(unrelated) == 5) result |= 16;
        if (null_throw(null) == 6) result |= 32;
        return result;
    }
    public static int original_checks() {
        bodies = 0;
        return target_int(expected) == 1 && bodies == 1 ? 1 : 0;
    }
    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 300000; ++i) { sink = target_int(expected); target_void(expected); target_object(expected); sync_resume(expected); sync_return(expected); instance.instance_throw(expected); }
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        for (;;) Thread.sleep(100);
    }
}

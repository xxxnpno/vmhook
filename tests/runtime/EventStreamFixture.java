public final class EventStreamFixture {
    public static final class Late {
        public static int target(int value) { return value + 1; }
    }
    public static final class QuietException extends RuntimeException {
        QuietException() { super(null, null, false, false); }
    }
    public static void load_late_class() { if (Late.target(1) != 2) throw new AssertionError(); }
    public static void stack_trace_events() {
        RuntimeException exception = new RuntimeException("fixture");
        exception.fillInStackTrace();
        try { throw exception; } catch (RuntimeException ignored) { }
        try { throw exception; } catch (RuntimeException ignored) { }
        new QuietException();
    }
    public static void main(String[] args) throws Exception {
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        for (;;) Thread.sleep(100);
    }
}

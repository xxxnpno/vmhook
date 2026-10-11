public final class HookCompositionFixture {
    private static volatile int sink;
    public static int target(int value) { return value + 1; }
    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 300000; ++i) sink = target(i);
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

public final class RootFixture {
    public static volatile int counter = 7;
    public static int target(int value) { return value + 1; }
    public static int read_counter() { return counter; }
    public static void main(String[] arguments) throws Exception {
        for (int index = 0; index < 300000; ++index) {
            target(index);
            read_counter();
        }
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

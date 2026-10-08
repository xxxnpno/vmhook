public class VMHookFixture {
    public static int total = 41;
    public static volatile boolean argumentMutation;
    public int value;
    public final String textValue = text();

    public VMHookFixture(int initial) {
        value = initial;
        System.gc();
    }
    public int add(int amount) {
        System.gc();
        return value += amount;
    }
    public static int count() { return total; }
    public static String text() { return "A\0\uD83D\uDE00\u00E9"; }
    public static String latin1() { return "caf\u00E9"; }
    public static int touch(VMHookFixture other) {
        System.gc();
        return other.value += 3;
    }
    public static int touchTwice(VMHookFixture left, VMHookFixture right) {
        System.gc();
        if (left != right) { throw new AssertionError("argument identity changed"); }
        left.value++;
        return right.value;
    }
    public static void fail() {
        System.gc();
        throw new IllegalStateException("expected fixture exception");
    }

    public static void tick() throws InterruptedException {
        Thread.sleep(25);
    }

    public int mixed(int first, long wide, int following, double floating, int last, Object[] array) {
        if (value == 15 && first == 21 && wide == 0x1122334455667788L
                && following == -3 && floating == 42.25 && last == -7 && array == null) {
            argumentMutation = true;
        }
        return value;
    }

    public static void main(String[] arguments) throws Exception {
        VMHookFixture fixture = new VMHookFixture(10);
        if (fixture.add(5) != 15 || count() != 41) {
            throw new AssertionError("fixture did not initialize");
        }
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        Object[] live = new Object[16];
        int iteration = 0;
        while (true) {
            live[iteration++ % live.length] = new byte[1024 * 1024];
            if (iteration % 20 == 0) { System.gc(); }
            fixture.mixed(3, 7L, 5, 6.5, 9, live);
            tick();
        }
    }
}

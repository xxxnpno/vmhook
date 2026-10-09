interface NativeDefault {
    default int defaultValue() { return 84; }
}

interface NativeOverride extends NativeDefault {
    @Override default int defaultValue() { return 85; }
}

class NativeBase {
    public NativeBase() {}
    public NativeBase(boolean ignored) {}
    public int virtualValue() { return 17; }
    private int privateValue() { return 17; }
    public static int staticValue() { return 17; }
}

public class NativeFixture extends NativeBase implements NativeOverride {
    public static final String TEXT = "A\0\uD83D\uDE00\u00E9";
    public static NativeFixture holder;
    public static volatile int gcEpoch;
    public int value;
    public String text;
    public NativeFixture child;

    public NativeFixture() {
        value = 41;
        text = TEXT;
        child = this;
    }

    public NativeFixture(int initial, String message, NativeFixture parent) {
        checkNativeThread();
        System.gc();
        if (!TEXT.equals(message)) { throw new AssertionError("String argument changed"); }
        value = initial;
        text = message;
        child = parent == null ? this : parent;
    }

    private static void checkNativeThread() {
        if (!Thread.currentThread().getName().startsWith("VMHook-native-")) {
            throw new AssertionError("Invocation ran on a Java-created fixture thread");
        }
    }

    public static String threadName() {
        checkNativeThread();
        return Thread.currentThread().getName();
    }

    public static int plain(int input) {
        checkNativeThread();
        return input + 1;
    }

    public int add(int amount) {
        checkNativeThread();
        System.gc();
        return value += amount;
    }

    public static NativeFixture touch(NativeFixture left, String message, NativeFixture right) {
        checkNativeThread();
        System.gc();
        if (left != right || !TEXT.equals(message)) {
            throw new AssertionError("Aliased or String arguments changed");
        }
        left.value++;
        return right;
    }

    public static String echo(String message) {
        checkNativeThread();
        System.gc();
        return message;
    }

    public static long wide(long first, double floating, int last) {
        checkNativeThread();
        if (floating != 42.25 || last != -7) { throw new AssertionError("Wide slots changed"); }
        return first;
    }

    public static double floating(float first, double second) {
        checkNativeThread();
        return first + second;
    }

    @Override public int virtualValue() { return 99; }
    public int privateValue() { return 99; }
    public static int staticValue() { return 99; }

    public static void fail() {
        checkNativeThread();
        System.gc();
        throw new IllegalStateException("expected native-call exception");
    }

    public static void collect() {
        checkNativeThread();
        System.gc();
    }

    public static class Lazy {
        private static final String initializedOn = Thread.currentThread().getName();
        public static String initializedOn() {
            checkNativeThread();
            return initializedOn;
        }
    }

    public static void main(String[] arguments) throws Exception {
        holder = new NativeFixture();
        Class.forName("NativeFixture$Lazy", false, NativeFixture.class.getClassLoader());
        Class.forName("java.lang.IllegalStateException");
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        Object[] live = new Object[16];
        int iteration = 0;
        while (true) {
            live[iteration++ % live.length] = new byte[1024 * 1024];
            if (iteration % 10 == 0) { System.gc(); ++gcEpoch; }
            Thread.sleep(20);
        }
    }
}

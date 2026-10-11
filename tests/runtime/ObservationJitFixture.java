import java.lang.management.ManagementFactory;

/** Ordinary tiered compilation, with no command-line compiler controls. */
public class ObservationJitFixture {
    public static volatile int input = 7;
    public static volatile long callerChanged;
    public static volatile long directChanged;
    public static volatile long futureChanged;
    public static volatile boolean futureRequested;
    public static volatile long normal;
    public static volatile long sink;
    public static volatile long longChanged;
    public static volatile long doubleChanged;
    public static volatile long floatChanged;
    public static volatile long referenceChanged;
    public static volatile long voidBodies;
    private static final Object REFERENCE = new Object();

    public static int target(int value) {
        // Preserve an inlined debug scope that the native probe can verify.
        if (value < 0) throw new IllegalArgumentException("negative");
        return value + 1;
    }

    public static int caller(int value) { return target(value) * 3; }
    public static int futureCaller(int value) { return target(value) * 5; }
    public static int unrelated(int value) { return (value * 5 + 9) ^ (value >>> 2); }
    public static long targetLong(long value) {
        if (value < 0) throw new IllegalArgumentException("negative long");
        return value + 1;
    }
    public static double targetDouble(double value) {
        if (value < 0) throw new IllegalArgumentException("negative double");
        return value + 1;
    }
    public static float targetFloat(float value) {
        if (value < 0) throw new IllegalArgumentException("negative float");
        return value + 1;
    }
    public static Object targetReference() { return REFERENCE; }
    public static void targetVoid() { ++voidBodies; }

    public static void main(String[] arguments) throws Exception {
        for (int iteration = 0; iteration < 2000000; ++iteration) {
            sink = caller(input) + target(input) + unrelated(input);
            sink ^= targetLong(0x1122334455667788L);
            sink += (long) targetDouble(42.25) + (long) targetFloat(7.5f);
            if (targetReference() == null) throw new AssertionError("warm reference");
            targetVoid();
        }
        Thread callers = new Thread(() -> {
            for (;;) {
                int value = input;
                int result = caller(value);
                if (result == (value + 100) * 3) ++callerChanged;
                else if (result == (value + 1) * 3) ++normal;
                else throw new AssertionError("caller result " + result);
                sink = unrelated(value);
                if (futureRequested) {
                    int future = futureCaller(value);
                    if (future == (value + 100) * 5) ++futureChanged;
                    else if (future != (value + 1) * 5) throw new AssertionError("future caller result " + future);
                }
            }
        }, "observation-compiled-caller");
        callers.setDaemon(true);
        callers.start();
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        for (;;) {
            int value = input;
            int result = target(value);
            if (result == value + 100) ++directChanged;
            else if (result != value + 1) throw new AssertionError("direct result " + result);
            sink = unrelated(value);
            long wide = targetLong(0x1122334455667788L);
            if (wide == 0x2233445566778899L) ++longChanged;
            else if (wide != 0x1122334455667789L) throw new AssertionError("long result " + wide);
            double floating = targetDouble(42.25);
            if (floating == 142.25) ++doubleChanged;
            else if (floating != 43.25) throw new AssertionError("double result " + floating);
            float narrow = targetFloat(7.5f);
            if (narrow == 107.5f) ++floatChanged;
            else if (narrow != 8.5f) throw new AssertionError("float result " + narrow);
            Object reference = targetReference();
            if (reference == null) ++referenceChanged;
            else if (reference != REFERENCE) throw new AssertionError("reference result");
            targetVoid();
        }
    }
}

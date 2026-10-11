import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.function.IntUnaryOperator;

public final class CoalescedJitFixture {
    private static volatile int input = 7;
    private static volatile int sink;
    private static final MethodHandle handle;
    private static final IntUnaryOperator operation = value -> target(value);
    private interface Callable { int apply(int value); }
    private static final Callable callable = new Callable() {
        public int apply(int value) { return target(value); }
    };
    static {
        try {
            handle = MethodHandles.lookup().findStatic(CoalescedJitFixture.class,
                "target", MethodType.methodType(int.class, int.class));
        } catch (ReflectiveOperationException failure) {
            throw new ExceptionInInitializerError(failure);
        }
    }
    public static int target(int value) {
        if (value < 0) throw new IllegalArgumentException("negative input");
        return value + 1;
    }
    public static int caller(int value) { return target(value) * 3; }
    public static int folded_target() { return 37; }
    public static int folded_caller(int value) { return folded_target() + value; }
    public static int unrelated(int value) {
        if (value < 0) throw new IllegalArgumentException("unrelated negative input");
        return (value * 5 + 9) ^ (value >>> 2);
    }
    public static int handle_caller(int value) throws Throwable { return (int)handle.invokeExact(value); }
    public static int interface_caller(int value) { return callable.apply(value); }
    public static int lambda_caller(int value) { return operation.applyAsInt(value); }
    public static void warm_up() throws Throwable {
        for (int index = 0; index < 2000000; ++index) {
            int value = input;
            sink = caller(value) + target(value) + unrelated(value)
                + folded_caller(value) + folded_target() + handle_caller(value)
                + interface_caller(value) + lambda_caller(value);
        }
    }
    public static void warm_unrelated() {
        for (int index = 0; index < 2000000; ++index) sink = unrelated(input);
    }
    public static void main(String[] arguments) throws Throwable {
        warm_up();
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

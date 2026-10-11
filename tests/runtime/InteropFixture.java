import java.lang.management.ManagementFactory;
import java.util.*;

public final class InteropFixture {
    public static final class Player {
        public final int id;
        public Player(int next) { id = next; }
    }
    public static final class Wide {
        public final long total;
        public Wide(int a, long b, double c, Player d, int e, long f, double g, Player h, String i, int j) {
            requireWorker();
            System.gc();
            if (a != 1 || b != 0x1234567812345678L || c != 3.25 || d.id != 11 || e != 5 ||
                f != -9000000000L || g != -7.5 || h.id != 22 || !i.equals("ninth") || j != 10) {
                throw new AssertionError("Mixed constructor arguments changed");
            }
            total = 1234;
        }
    }
    static void requireWorker() {
        if (!Thread.currentThread().getName().startsWith("VMHook-interop-")) {
            throw new AssertionError("Call left the native worker");
        }
    }
    public static long mixed(int a, long b, double c, Player d, int e, long f, double g, Player h, String i, int j) {
        requireWorker();
        System.gc();
        if (a != 1 || b != 0x1234567812345678L || c != 3.25 || d.id != 11 || e != 5 ||
            f != -9000000000L || g != -7.5 || h.id != 22 || !i.equals("ninth") || j != 10) {
            throw new AssertionError("Mixed method arguments changed");
        }
        return 1234;
    }
    public static Throwable throwable() { requireWorker(); return new IllegalStateException("interop-error"); }
    public static String surrogate() { requireWorker(); return "\ud800X"; }
    public static void collect() { requireWorker(); System.gc(); }
    public static void verifyPrimitive(Object value, String descriptor, int length) {
        requireWorker();
        if (!value.getClass().getName().equals(descriptor) || java.lang.reflect.Array.getLength(value) != length) {
            throw new AssertionError("Array class or length changed");
        }
    }
    public static Player[] objectArray(Player first, Player last) {
        requireWorker();
        return new Player[] { first, null, last };
    }
    public static void main(String[] args) throws Exception {
        // Ensure only application classes are loaded; Array and unused primitive
        // wrappers may be loaded by VMHook through bootstrap Class.forName.
        new Player(0);
        Class.forName("InteropFixture$Wide", false, InteropFixture.class.getClassLoader());
        new ArrayList<Object>(); new HashMap<Object,Object>(); new HashSet<Object>(); new ArrayDeque<Object>();
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY"); System.out.flush();
        for (;;) { Thread.sleep(100); }
    }
}

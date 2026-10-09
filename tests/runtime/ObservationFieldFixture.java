import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.lang.management.ManagementFactory;
import java.lang.reflect.Method;

class ObservationFieldFuture {
    public static int read() { return ObservationFieldFixture.intValue; }
    public static void write(int value) { ObservationFieldFixture.intValue = value; }
}

public class ObservationFieldFixture {
    public static volatile int intValue = 17;
    public static volatile long longValue = 0x1122334455667788L;
    public static volatile double doubleValue = 42.25;
    public static int plainValue = 91;
    public static volatile byte byteValue = -7;
    public static volatile short shortValue = -301;
    public static volatile char charValue = 'A';
    public static volatile boolean boolValue;
    public static volatile float floatValue = 42.25f;
    public static Object referenceValue;
    public static volatile int gcEpoch;
    public static volatile long sink;

    private static void check(boolean valid, String label) {
        if (!valid) throw new AssertionError(label);
    }

    public static long warmedAccessor() {
        return intValue + longValue + (long) doubleValue + plainValue
            + byteValue + shortValue + charValue + (boolValue ? 1 : 0) + (long) floatValue;
    }

    public static void exercise() {
        check(intValue == 117, "int read replacement");
        intValue = 29;
        check(intValue == 229, "int write replacement");
        intValue = 333;
        check(intValue == 229, "int write cancellation");
        check(longValue == 0x2233445566778899L, "long read replacement");
        longValue = 123;
        check(longValue == 223, "long write replacement");
        longValue = 333;
        check(longValue == 223, "long write cancellation");
        check(doubleValue == 142.25, "double read replacement");
        doubleValue = 23.5;
        check(doubleValue == 223.5, "double write replacement");
        doubleValue = 333.5;
        check(doubleValue == 223.5, "double write cancellation");
        check(plainValue == 0, "canceled read default value");
        check(byteValue == -9, "byte read replacement");
        byteValue = 20;
        check(byteValue == -30, "byte write replacement");
        byteValue = 33;
        check(byteValue == -30, "byte write cancellation");
        check(shortValue == -309, "short read replacement");
        shortValue = 20;
        check(shortValue == -3000, "short write replacement");
        shortValue = 33;
        check(shortValue == -3000, "short write cancellation");
        check(charValue == 'Q', "char read replacement");
        charValue = 'R';
        check(charValue == 'Z', "char write replacement");
        charValue = 'S';
        check(charValue == 'Z', "char write cancellation");
        check(boolValue, "boolean read replacement");
        boolValue = true;
        check(!boolValue, "boolean write replacement");
        boolValue = false;
        check(!boolValue, "boolean write cancellation");
        check(floatValue == 142.25f, "float read replacement");
        floatValue = 23.5f;
        check(floatValue == 223.5f, "float write replacement");
        floatValue = 333.5f;
        check(floatValue == 223.5f, "float write cancellation");
    }

    private static Class<?> futureAccessor() throws Exception {
        final byte[] bytes;
        try (InputStream in = ObservationFieldFixture.class.getResourceAsStream("ObservationFieldFuture.class");
             ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[1024];
            for (int size; (size = in.read(buffer)) != -1;) out.write(buffer, 0, size);
            bytes = out.toByteArray();
        }
        class Loader extends ClassLoader {
            Class<?> define() { return defineClass(null, bytes, 0, bytes.length); }
        }
        return new Loader().define();
    }

    private static void mark(File path) throws Exception {
        try (FileOutputStream output = new FileOutputStream(path)) { output.write(1); }
    }

    private static void await(File path) throws Exception {
        while (!path.exists()) Thread.sleep(10);
    }

    public static void main(String[] arguments) throws Exception {
        for (int iteration = 0; iteration < 2000000; ++iteration) sink = warmedAccessor();
        Thread collector = new Thread(() -> {
            Object[] live = new Object[4];
            for (int iteration = 0;; ++iteration) {
                live[iteration % live.length] = new byte[1024 * 1024];
                System.gc();
                ++gcEpoch;
                try { Thread.sleep(1); } catch (InterruptedException ignored) {}
            }
        }, "observation-field-gc");
        collector.setDaemon(true);
        // A cold sleep can leave a discarded JFR event Klass awaiting metadata
        // cleanup. Install first, before an explicit GC can remove that copy.
        Thread.sleep(1);
        File control = new File(System.getenv("VMHOOK_OBSERVATION_CONTROL"));
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        await(control);
        collector.start();
        exercise();
        Class<?> future = futureAccessor();
        Method read = future.getDeclaredMethod("read");
        Method write = future.getDeclaredMethod("write", int.class);
        read.setAccessible(true);
        write.setAccessible(true);
        write.invoke(null, 44);
        for (int iteration = 0; iteration < 20000; ++iteration) {
            check(((Integer) read.invoke(null)) == 244, "future class read");
        }
        write.invoke(null, 333);
        check(((Integer) read.invoke(null)) == 244, "future class write cancellation");
        mark(new File(control.getPath() + ".done"));
        await(new File(control.getPath() + ".restore"));
        intValue = 17;
        longValue = 123;
        doubleValue = 42.25;
        plainValue = 91;
        byteValue = -7;
        shortValue = -301;
        charValue = 'A';
        boolValue = true;
        floatValue = 42.25f;
        check(intValue == 17 && longValue == 123 && doubleValue == 42.25 && plainValue == 91
            && byteValue == -7 && shortValue == -301 && charValue == 'A' && boolValue
            && floatValue == 42.25f, "shutdown restores bytecode behavior");
        mark(new File(control.getPath() + ".restore.done"));
        System.out.println("FIELDS=pass");
        System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

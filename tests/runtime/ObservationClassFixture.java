import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.Array;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

class ObservationEventTemplate { public int value() { return 42; } }
class ObservationEventBuffer { public int value() { return 43; } }
class ObservationEventHidden { public int value() { return 44; } }
class ObservationEventAfterRemove { public int value() { return 45; } }
class ObservationEventAfterShutdown { public int value() { return 46; } }

public class ObservationClassFixture {
    static class Loader extends ClassLoader {
        Loader() { super(null); }
        Class<?> define(byte[] bytes) { return defineClass(null, bytes, 0, bytes.length); }
        Class<?> defineBuffer(byte[] bytes) { return defineClass(null, ByteBuffer.wrap(bytes), null); }
    }

    static byte[] bytes(String name) throws Exception {
        try (InputStream input = ObservationClassFixture.class.getResourceAsStream(name + ".class");
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[1024];
            for (int size; (size = input.read(buffer)) != -1;) output.write(buffer, 0, size);
            return output.toByteArray();
        }
    }

    private static void mark(File path) throws Exception {
        try (FileOutputStream output = new FileOutputStream(path)) { output.write(1); }
    }

    private static void await(File path) throws Exception {
        while (!path.exists()) Thread.sleep(10);
    }

    public static void main(String[] arguments) throws Exception {
        byte[] template = bytes("ObservationEventTemplate");
        byte[] buffer = bytes("ObservationEventBuffer");
        byte[] hidden = bytes("ObservationEventHidden");
        byte[] afterRemove = bytes("ObservationEventAfterRemove");
        byte[] afterShutdown = bytes("ObservationEventAfterShutdown");
        File control = new File(System.getenv("VMHOOK_OBSERVATION_CONTROL"));
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        await(control);
        Loader one = new Loader();
        Loader two = new Loader();
        Class<?> first = one.define(template);
        Class<?> second = two.define(template);
        if (first == second) throw new AssertionError("loader identity collapsed");
        one.defineBuffer(buffer);
        try {
            one.define(template);
            throw new AssertionError("duplicate accepted");
        } catch (LinkageError expected) {}
        Array.newInstance(first, 1, 1);
        Array.newInstance(first, 1, 1);
        Array.newInstance(second, 1);
        int[][] primitive = new int[1][1];
        if (primitive.length != 1) throw new AssertionError("primitive array");
        try {
            Class<?> option = Class.forName("java.lang.invoke.MethodHandles$Lookup$ClassOption");
            Object options = Array.newInstance(option, 0);
            MethodHandles.Lookup.class.getMethod("defineHiddenClass", byte[].class, boolean.class, options.getClass())
                .invoke(MethodHandles.lookup(), hidden, false, options);
        } catch (ClassNotFoundException legacy) {
            Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
            java.lang.reflect.Field field = unsafeClass.getDeclaredField("theUnsafe");
            field.setAccessible(true);
            Object unsafe = field.get(null);
            unsafeClass.getMethod("defineAnonymousClass", Class.class, byte[].class, Object[].class)
                .invoke(unsafe, ObservationClassFixture.class, hidden, null);
        }
        Class.forName("javax.crypto.spec.PBEParameterSpec");
        List<Thread> workers = new ArrayList<Thread>();
        for (int index = 0; index < 4; ++index) {
            Thread worker = new Thread(() -> { new Loader().define(template); }, "observation-loader-" + index);
            workers.add(worker);
            worker.start();
        }
        for (Thread worker : workers) worker.join();
        mark(new File(control.getPath() + ".done"));
        await(new File(control.getPath() + ".remove"));
        new Loader().define(afterRemove);
        mark(new File(control.getPath() + ".remove.done"));
        await(new File(control.getPath() + ".shutdown"));
        new Loader().define(afterShutdown);
        mark(new File(control.getPath() + ".shutdown.done"));
        System.out.println("CLASSES=pass");
        System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

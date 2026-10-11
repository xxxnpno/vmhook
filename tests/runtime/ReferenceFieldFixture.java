import java.io.File;
import java.io.InputStream;
import java.io.ByteArrayOutputStream;
import java.lang.management.ManagementFactory;

public final class ReferenceFieldFixture {
    public static final class Player {
        public final int id;
        public Player(int value) { id = value; }
    }
    public static volatile Player reference = new Player(11);
    public static volatile Player prevented = new Player(12);
    public static volatile Player[] references = new Player[]{new Player(13)};
    public static final class Holder {
        public static volatile Player nested = new Player(14);
        public static synchronized Player read() { return nested; }
    }
    private static final class CopyLoader extends ClassLoader {
        CopyLoader() { super(ReferenceFieldFixture.class.getClassLoader()); }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            if (!name.equals(Player.class.getName()) && !name.equals(Holder.class.getName())) {
                return super.loadClass(name, resolve);
            }
            Class<?> loaded = findLoadedClass(name);
            if (loaded == null) {
                try (InputStream input = getParent().getResourceAsStream(name.replace('.', '/') + ".class")) {
                    if (input == null) throw new ClassNotFoundException(name);
                    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                    byte[] buffer = new byte[4096]; int count;
                    while ((count = input.read(buffer)) != -1) bytes.write(buffer, 0, count);
                    byte[] code = bytes.toByteArray(); loaded = defineClass(name, code, 0, code.length);
                } catch (java.io.IOException failure) { throw new ClassNotFoundException(name, failure); }
            }
            if (resolve) resolveClass(loaded);
            return loaded;
        }
    }
    private static Object second_player;
    private static Object second_holder;
    private static Object second_array;
    public static Object first_player() { return reference; }
    public static Object wrong_player() { return second_player; }
    public static Object wrong_holder() { return second_holder; }
    public static int nested_id() throws Exception {
        Object value = second_holder.getClass().getMethod("read").invoke(null);
        return value.getClass().getField("id").getInt(value);
    }
    public static volatile int sink;
    public static synchronized Player read() { return reference; }
    public static synchronized void write(Player value) { reference = value; }
    public static synchronized Player[] array_read() { return references; }
    public static synchronized void array_write(Player[] value) { references = value; }
    public static void collect() { System.gc(); }
    public static Player actual() throws Exception {
        return (Player)ReferenceFieldFixture.class.getDeclaredField("reference").get(null);
    }
    public static Player[] actual_array() throws Exception {
        return (Player[])ReferenceFieldFixture.class.getDeclaredField("references").get(null);
    }
    private static void require(boolean value, String label) {
        if (!value) throw new AssertionError(label);
    }
    public static void main(String[] args) throws Exception {
        CopyLoader copy = new CopyLoader();
        second_player = copy.loadClass(Player.class.getName()).getConstructor(int.class).newInstance(333);
        second_holder = copy.loadClass(Holder.class.getName()).getConstructor().newInstance();
        // Make both same-name array Classes exist before native installation.
        second_array = java.lang.reflect.Array.newInstance(second_player.getClass(), 0);
        for (int i = 0; i < 1000000; ++i) { sink = read().id; write(reference); }
        File control = new File(System.getenv("VMHOOK_REFERENCE_CONTROL"));
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY"); System.out.flush();
        while (!control.exists()) Thread.sleep(10);
        Thread collector = new Thread(() -> {
            Object[] live = new Object[4];
            for (int i = 0;; ++i) {
                live[i % live.length] = new byte[1024 * 1024]; System.gc();
                try { Thread.sleep(1); } catch (InterruptedException ignored) {}
            }
        }, "reference-field-GC");
        collector.setDaemon(true); collector.start();
        for (int i = 0; i < 200; ++i) {
            require(read().id == 101, "reference read replacement");
            write(new Player(21));
            require(actual().id == 121, "reference write replacement");
            write(new Player(33));
            require(actual().id == 121, "reference write prevention");
            write(null);
            require(actual() == null, "Java null write");
            require(prevented == null, "prevented reference read is Java null");
            if (i < 40) {
                require(array_read()[0].id == 101, "array read replacement after binding rebind");
                array_write(new Player[]{new Player(21)});
                require(actual_array()[0].id == 121, "array write replacement");
                array_write(new Player[]{new Player(33)});
                require(actual_array()[0].id == 121, "array write prevention");
                array_write(null);
                require(actual_array() == null, "array Java null write");
            }
        }
        System.out.println("REFERENCE_FIELDS=pass"); System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

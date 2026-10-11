import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.lang.ref.WeakReference;

/** Independent-loader and cross-context root regression fixture. */
public final class LifetimeFixture {
    public static final class Duplicate {
        public static int total = 100;
        public int value;
        public Duplicate(int next) { value = next; total += next; }
        public int add(int amount) { return value + amount; }
        public static int total_value() { return total; }
    }

    private static final class IsolatedLoader extends ClassLoader {
        IsolatedLoader() { super(LifetimeFixture.class.getClassLoader()); }
        protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            if (!name.equals("LifetimeFixture$Duplicate")) { return super.loadClass(name, resolve); }
            Class<?> selected = findLoadedClass(name);
            if (selected == null) {
                try (InputStream source = getParent().getResourceAsStream("LifetimeFixture$Duplicate.class")) {
                    if (source == null) { throw new ClassNotFoundException(name); }
                    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                    byte[] buffer = new byte[4096];
                    for (int count; (count = source.read(buffer)) >= 0;) { bytes.write(buffer, 0, count); }
                    byte[] code = bytes.toByteArray();
                    selected = defineClass(name, code, 0, code.length);
                } catch (java.io.IOException failure) { throw new ClassNotFoundException(name, failure); }
            }
            if (resolve) { resolveClass(selected); }
            return selected;
        }
    }

    private static Object first;
    private static Object second;
    private static ClassLoader first_loader;
    private static WeakReference<Object> first_weak;

    public static Object first() { return first; }
    public static Object second() { return second; }
    public static ClassLoader first_loader() { return first_loader; }
    public static boolean current_loader_matches(ClassLoader expected) {
        return Thread.currentThread().getContextClassLoader() == expected;
    }
    public static void drop_anchors() { first = null; second = null; first_loader = null; }
    public static boolean first_alive() { return first_weak.get() != null; }
    public static void collect() { System.gc(); }

    public static void main(String[] arguments) throws Exception {
        first_loader = new IsolatedLoader();
        ClassLoader other_loader = new IsolatedLoader();
        first = first_loader.loadClass("LifetimeFixture$Duplicate").getConstructor(int.class).newInstance(41);
        second = other_loader.loadClass("LifetimeFixture$Duplicate").getConstructor(int.class).newInstance(73);
        first_weak = new WeakReference<Object>(first);
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        while (true) {
            byte[][] garbage = new byte[64][];
            for (int index = 0; index < garbage.length; ++index) { garbage[index] = new byte[32768]; }
            System.gc();
            Thread.sleep(20);
        }
    }
}

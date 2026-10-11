import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.lang.management.ManagementFactory;
import java.lang.reflect.Array;
import java.lang.reflect.Method;

/** Same-name method receiver, argument and nested-array loader regression. */
public final class MethodBindingFixture {
    public static final class Player {
        public final int id;
        public Player(int value) { id = value; }
    }

    public static final class Holder {
        public final int id;
        public Holder(int value) { id = value; }
        public synchronized int accept(Player value, Player[][] matrix) {
            return id + (value == null ? 0 : value.id)
                + (matrix == null ? 0 : matrix[0][0].id);
        }
        public synchronized int accept_any(Object value) {
            return id + (value == null ? 0 : 1);
        }
        public synchronized int accept_player(Player value) {
            return id + (value == null ? 0 : value.id);
        }
    }

    private static final class CopyLoader extends ClassLoader {
        CopyLoader() { super(MethodBindingFixture.class.getClassLoader()); }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve)
            throws ClassNotFoundException {
            if (!name.equals("MethodBindingFixture$Player")
                && !name.equals("MethodBindingFixture$Holder")) {
                return super.loadClass(name, resolve);
            }
            Class<?> selected = findLoadedClass(name);
            if (selected == null) {
                try (InputStream source = getParent().getResourceAsStream(name.replace('.', '/') + ".class")) {
                    if (source == null) throw new ClassNotFoundException(name);
                    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                    byte[] buffer = new byte[4096];
                    for (int count; (count = source.read(buffer)) != -1;) bytes.write(buffer, 0, count);
                    byte[] code = bytes.toByteArray();
                    selected = defineClass(name, code, 0, code.length);
                } catch (java.io.IOException failure) {
                    throw new ClassNotFoundException(name, failure);
                }
            }
            if (resolve) resolveClass(selected);
            return selected;
        }
    }

    private static final Player first_player = new Player(41);
    private static final Holder first_holder = new Holder(11);
    private static final Player[][] first_matrix = new Player[][]{{first_player}};
    private static Object second_player;
    private static Object second_holder;
    private static Object second_matrix;
    private static Method second_accept;
    private static volatile int sink;

    public static Object first_player() { return first_player; }
    public static Object second_player() { return second_player; }
    public static Object first_holder() { return first_holder; }
    public static Object second_holder() { return second_holder; }
    public static int run_a() { return first_holder.accept(first_player, first_matrix); }
    public static int run_a_null() { return first_holder.accept(null, null); }
    public static int run_player() { return first_holder.accept_player(first_player); }
    public static int run_b() throws Exception {
        return ((Integer)second_accept.invoke(second_holder, second_player, second_matrix)).intValue();
    }
    public static void collect() { System.gc(); }

    public static void main(String[] arguments) throws Exception {
        CopyLoader copy = new CopyLoader();
        Class<?> player_type = copy.loadClass("MethodBindingFixture$Player");
        Class<?> holder_type = copy.loadClass("MethodBindingFixture$Holder");
        second_player = player_type.getConstructor(int.class).newInstance(73);
        second_holder = holder_type.getConstructor(int.class).newInstance(22);
        second_matrix = Array.newInstance(player_type, new int[]{1, 1});
        Array.set(Array.get(second_matrix, 0), 0, second_player);
        second_accept = holder_type.getMethod("accept", player_type, second_matrix.getClass());
        if (first_player.getClass() == second_player.getClass()
            || first_holder.getClass() == second_holder.getClass()) {
            throw new AssertionError("duplicate defining loaders were not established");
        }
        // Warm the ordinary application-loader call and its synchronized target.
        // Hook admission must invalidate already compiled and inlined callers.
        for (int index = 0; index < 500000; ++index) sink = run_a();
        if (run_a() != 93 || run_b() != 168 || run_a_null() != 11 || run_player() != 52) {
            throw new AssertionError("original method results");
        }
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

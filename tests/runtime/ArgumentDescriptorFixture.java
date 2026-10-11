import java.lang.management.ManagementFactory;
import java.util.*;

public final class ArgumentDescriptorFixture {
    public static final class Player { public final int id; public Player(int value) { id = value; } }
    static void collect() { System.gc(); }
    static void validate(Object value) {
        if (!Thread.currentThread().getName().startsWith("VMHook-descriptors"))
            throw new AssertionError("Constructor left native OS worker");
        System.gc();
        if (value instanceof Player[] && ((Player[])value)[0].id != 11)
            throw new AssertionError("Array argument changed across collection");
    }
    public static final class StringExact {
        public final int marker;
        public StringExact(Object value) { validate(value); marker = 1; }
        public StringExact(String value) { validate(value); marker = 2; }
    }
    public static final class StringBroad {
        public final int marker;
        public StringBroad(Object value) { validate(value); marker = 1; }
        public StringBroad(CharSequence value) { validate(value); marker = 3; }
    }
    public static final class StringObject {
        public final int marker;
        public StringObject(Object value) { validate(value); marker = 4; }
    }
    public static final class ArrayExact {
        public final int marker;
        public ArrayExact(Object value) { validate(value); marker = 1; }
        public ArrayExact(Object[] value) { validate(value); marker = 5; }
        public ArrayExact(Player[] value) { validate(value); marker = 6; }
    }
    public static final class ArrayBroad {
        public final int marker;
        public ArrayBroad(Object value) { validate(value); marker = 1; }
        public ArrayBroad(Object[] value) { validate(value); marker = 5; }
    }
    public static final class ArrayObject {
        public final int marker;
        public ArrayObject(Object value) { validate(value); marker = 4; }
    }
    public static final class Nested {
        public final int marker;
        public Nested(int[][] value) {
            validate(value);
            if (value != null && value[0][0] != 42) throw new AssertionError("Nested argument changed");
            marker = 7;
        }
    }
    public static final class CollectionExact {
        public final int marker;
        public CollectionExact(Object value) { validate(value); marker = 1; }
        public CollectionExact(Collection<?> value) { validate(value); marker = 8; }
    }
    public static final class CollectionBroad {
        public final int marker;
        public CollectionBroad(Object value) { validate(value); marker = 1; }
        public CollectionBroad(Collection<?> value) { validate(value); marker = 9; }
    }
    public static final class Invalid {
        public final int marker;
        public Invalid(Player value) { validate(value); marker = -1; }
    }
    public static final class Ambiguous {
        public final int marker;
        public Ambiguous(CharSequence value) { validate(value); marker = -1; }
        public Ambiguous(java.io.Serializable value) { validate(value); marker = -2; }
    }
    public static Collection<Object> get_collection() { return new ArrayList<Object>(); }
    public static int array_target(Player[] value) { return value == null ? -1 : value[0].id; }
    public static int nested_target(int[][] value) { return value == null ? -1 : value[0][0]; }
    public static int collection_target(Collection<?> value) { return value == null ? -1 : value.size(); }
    public static void main(String[] args) throws Exception {
        for (Class<?> type : ArgumentDescriptorFixture.class.getDeclaredClasses())
            Class.forName(type.getName(), false, ArgumentDescriptorFixture.class.getClassLoader());
        final Player[] players = new Player[] { new Player(11) };
        final int[][] nested = new int[][] { new int[] { 42 } };
        final Collection<Object> collection = new ArrayList<Object>();
        for (int i = 0; i < 1000000; ++i) {
            array_target(players); nested_target(nested); collection_target(collection);
        }
        Thread gc = new Thread(new Runnable() { public void run() {
            for (;;) { byte[] allocation = new byte[65536]; allocation[0] = 1;
                try { Thread.sleep(2); } catch (InterruptedException ignored) { return; } }
        }}, "descriptor-gc");
        gc.setDaemon(true); gc.start();
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY"); System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

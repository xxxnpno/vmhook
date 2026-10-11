import java.util.AbstractCollection;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.concurrent.ConcurrentHashMap;

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
    public static CollectionCases collectionCases;
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

    public interface PlayerView {
        int id();
    }

    public static class Player implements Comparable<Player>, PlayerView {
        public final int id;
        public Player(int value) { id = value; }
        public int id() { return id; }
        public int compareTo(Player other) { return Integer.compare(id, other.id); }
    }

    public static final class SubPlayer extends Player {
        public SubPlayer(int value) { super(value); }
    }

    public static final class Key implements Comparable<Key> {
        public final int id;
        public Key(int value) { id = value; }
        public int compareTo(Key other) { return Integer.compare(id, other.id); }
        @Override public boolean equals(Object other) {
            return other instanceof Key && id == ((Key) other).id;
        }
        @Override public int hashCode() { return id; }
    }

    public static final class MovingCollection extends AbstractCollection<Player> {
        private final List<Player> values;
        public MovingCollection(Player... elements) { values = Arrays.asList(elements); }
        public Iterator<Player> iterator() { return values.iterator(); }
        public int size() { return values.size(); }
        @Override public Object[] toArray() {
            checkNativeThread();
            ++collectionCases.customArrayCalls;
            System.gc();
            return values.toArray();
        }
    }

    public static final class ThrowingCollection extends AbstractCollection<Player> {
        public Iterator<Player> iterator() { return Collections.<Player>emptyList().iterator(); }
        public int size() { return 0; }
        @Override public Object[] toArray() {
            checkNativeThread();
            throw new IllegalStateException("expected collection conversion exception");
        }
    }

    public static final class CollectionCases {
        public final ArrayList<Player> arrayList;
        public final LinkedList<Player> linkedList;
        public final HashSet<Player> hashSet;
        public final LinkedHashSet<Player> linkedHashSet;
        public final TreeSet<Player> treeSet;
        public final ArrayDeque<Player> arrayDeque;
        public final Collection<Player> unmodifiableCollection;
        public final List<Player> unmodifiableList;
        public final Collection<Player> customCollection;
        public final Collection<Player> throwingCollection = new ThrowingCollection();
        public final Object[] objectArray;
        public final Object objectArrayAsObject;
        public final int[] primitiveArray = {1, 2, 3};
        public final Object[][] nestedArray = {{new Player(6)}};
        public final Collection<Object> mixedCollection;
        public final Object plainObject = new Object();
        public final int primitive = 17;
        public final Collection<Player> nullCollection = null;
        public final Map<Key, Player> nullMap = null;
        public final Collection<Player> emptyCollection = Collections.emptyList();
        public final Map<Key, Player> emptyMap = Collections.emptyMap();
        public final HashMap<Key, Player> hashMap = new HashMap<Key, Player>();
        public final LinkedHashMap<Key, Player> linkedHashMap = new LinkedHashMap<Key, Player>();
        public final TreeMap<Key, Player> treeMap = new TreeMap<Key, Player>();
        public final IdentityHashMap<Key, Player> identityHashMap = new IdentityHashMap<Key, Player>();
        public final ConcurrentHashMap<Key, Player> concurrentHashMap = new ConcurrentHashMap<Key, Player>();
        public final Map<Key, Player> unmodifiableMap;
        public final Map<Integer, Player> wrongKeyMap = new HashMap<Integer, Player>();
        public final Map<Key, Object> wrongValueMap = new HashMap<Key, Object>();
        public int customArrayCalls;

        public CollectionCases() {
            Player first = new Player(1);
            Player second = new Player(2);
            Player third = new SubPlayer(3);
            arrayList = new ArrayList<Player>(Arrays.asList(first, null, second, third));
            linkedList = new LinkedList<Player>(Arrays.asList(third, first, null));
            hashSet = new HashSet<Player>(Arrays.asList(second, first, null));
            linkedHashSet = new LinkedHashSet<Player>(Arrays.asList(second, first, null, second));
            treeSet = new TreeSet<Player>(Arrays.asList(third, second, first));
            arrayDeque = new ArrayDeque<Player>(Arrays.asList(third, first, second));
            unmodifiableCollection = Collections.unmodifiableCollection(arrayList);
            unmodifiableList = Collections.unmodifiableList(arrayList);
            customCollection = new MovingCollection(second, null, first);
            objectArray = new Object[] {third, null, first};
            objectArrayAsObject = objectArray;
            mixedCollection = new ArrayList<Object>(Arrays.asList(first, new Object(), second));
            linkedHashMap.put(new Key(2), new Player(20));
            linkedHashMap.put(null, new Player(30));
            linkedHashMap.put(new Key(1), null);
            hashMap.putAll(linkedHashMap);
            treeMap.put(new Key(2), new Player(20));
            treeMap.put(new Key(1), null);
            identityHashMap.put(new Key(7), new Player(70));
            identityHashMap.put(new Key(7), new Player(71));
            concurrentHashMap.put(new Key(8), new Player(80));
            concurrentHashMap.put(new Key(9), new Player(90));
            unmodifiableMap = Collections.unmodifiableMap(linkedHashMap);
            wrongKeyMap.put(Integer.valueOf(1), first);
            wrongValueMap.put(new Key(1), new Object());
        }
    }

    public static Collection<Player> temporaryCollection() {
        checkNativeThread();
        Collection<Player> result = new ArrayList<Player>(Arrays.asList(new Player(81), null, new Player(82)));
        System.gc();
        return result;
    }

    public static Map<Key, Player> temporaryMap() {
        checkNativeThread();
        Map<Key, Player> result = new LinkedHashMap<Key, Player>();
        result.put(new Key(91), new Player(910));
        result.put(null, null);
        System.gc();
        return result;
    }

    public static Object temporaryArrayAsObject() {
        checkNativeThread();
        Object result = new Object[] {new Player(83), null, new Player(84)};
        System.gc();
        return result;
    }

    public static Collection<Player> nullCollection() {
        checkNativeThread();
        return null;
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
        collectionCases = new CollectionCases();
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

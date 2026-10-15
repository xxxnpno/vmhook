import java.lang.management.ManagementFactory;
import java.lang.ref.WeakReference;
import java.util.AbstractCollection;
import java.util.AbstractMap;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.NoSuchElementException;

/** Ordinary Java 8 application objects; no VM options or agent assistance. */
public class CollectionViewsFixture {
    public static final class Player {
        public final int id;
        public Player(int id) { this.id = id; }
    }
    public static final RuntimeException iterator_failure = new IllegalArgumentException("iterator marker");
    public static final RuntimeException has_next_failure = new IllegalStateException("hasNext marker");
    public static final RuntimeException next_failure = new UnsupportedOperationException("next marker");
    public static volatile int iterator_calls, has_next_calls, next_calls, gc_calls;
    public static volatile boolean live_roots = true;
    public static final Iterable<Object> custom = new Source(0, 3);
    public static final Iterable<Object> null_iterable = null;
    public interface DefaultIterator extends Iterator<Object> {
        int index();
        void advance();
        default boolean hasNext() { return index() == 0; }
        default Object next() {
            if (!hasNext()) throw new NoSuchElementException();
            advance(); return new Player(77);
        }
    }
    public static final class DefaultCursor implements DefaultIterator {
        int index;
        public int index() { return index; }
        public void advance() { ++index; }
    }
    public interface DefaultIterable extends Iterable<Object> {
        default Iterator<Object> iterator() { return new DefaultCursor(); }
    }
    public static final class Defaults implements DefaultIterable { }
    public static final Iterable<Object> defaults = new Defaults();
    public static final AbstractCollection<Object> snapshot_only = new AbstractCollection<Object>() {
        public int size() { return 3; }
        public Iterator<Object> iterator() { throw new AssertionError("Collection.iterator must not be used"); }
        public Object[] toArray() { collect(); return new Object[]{new Player(41), null, new Player(43)}; }
    };
    public static final Map<Object, Object> values = new LinkedHashMap<Object, Object>();
    public static final Map.Entry<Object, Object> live_entry;
    public static final Map.Entry<Object, Object> null_entry = new AbstractMap.SimpleEntry<Object, Object>(null, null);
    public static final Map.Entry<Object, Object> immutable_entry = new AbstractMap.SimpleImmutableEntry<Object, Object>(new Player(8), new Player(9));
    static {
        values.put(new Player(5), new Player(6));
        live_entry = values.entrySet().iterator().next();
    }
    public static final class Source implements Iterable<Object> {
        final int kind, size;
        Source(int kind, int size) { this.kind = kind; this.size = size; }
        public Iterator<Object> iterator() {
            ++iterator_calls;
            if (kind == 1) return null;
            if (kind == 2) throw iterator_failure;
            if (kind == 7) {
                final ArrayList<Object> list = new ArrayList<Object>();
                list.add(new Player(1)); list.add(new Player(2));
                final Iterator<Object> cursor = list.iterator();
                return new Iterator<Object>() {
                    boolean changed;
                    public boolean hasNext() { ++has_next_calls; return cursor.hasNext(); }
                    public Object next() {
                        ++next_calls;
                        Object value = cursor.next();
                        if (!changed) { list.add(new Player(3)); changed = true; }
                        return value;
                    }
                    public void remove() { throw new UnsupportedOperationException(); }
                };
            }
            return new Cursor(this);
        }
    }
    public static final class Cursor implements Iterator<Object> {
        final Source source;
        final WeakReference<Source> source_check;
        final WeakReference<Cursor> self_check;
        int index;
        Cursor(Source source) {
            this.source = source;
            source_check = new WeakReference<Source>(source);
            self_check = new WeakReference<Cursor>(this);
        }
        public boolean hasNext() {
            ++has_next_calls; collect();
            live_roots &= source_check.get() == source && self_check.get() == this;
            if (source.kind == 3 && index == 1) throw has_next_failure;
            return source.kind == 6 || index < source.size;
        }
        public Object next() {
            ++next_calls; collect();
            if (source.kind == 4 && index == 1) throw next_failure;
            if (source.kind != 6 && index >= source.size) throw new NoSuchElementException();
            int current = index++;
            if (source.kind == 5 && current == 2) return "wrong element";
            return current == 1 ? null : new Player(current + 11);
        }
        public void remove() { throw new UnsupportedOperationException(); }
    }
    public static Iterable<Object> make_iterable(int kind, int size) {
        iterator_calls = has_next_calls = next_calls = 0;
        return new Source(kind, size);
    }
    public static Iterable<Object> temporary() { return make_iterable(0, 3); }
    public static int counter(int index) {
        return index == 0 ? iterator_calls : index == 1 ? has_next_calls : next_calls;
    }
    public static boolean same_failure(Object value, int index) {
        return value == (index == 0 ? iterator_failure : index == 1 ? has_next_failure : next_failure);
    }
    public static int mapped_id() { return ((Player)values.values().iterator().next()).id; }
    public static void collect() { System.gc(); ++gc_calls; }
    public static void main(String[] args) throws Exception {
        // Load every fixture class before the injected DLL resolves bindings.
        new Cursor(new Source(0, 0)); new DefaultCursor();
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY"); System.out.flush();
        for (;;) Thread.sleep(1000);
    }
}

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class ExampleFixture {
    public int value;
    public Player selected;
    public final List<Player> live_players = new ArrayList<Player>();
    public static int total = 100;
    public static final List<Player> players = Arrays.asList(new Player(7), new Player(11), null);
    private static volatile long blackhole;

    public ExampleFixture(int initial) { value = initial; }
    public int add(int amount) { return value += amount; }
    public static String thread_name() { return Thread.currentThread().getName(); }
    public static Player echo(Player player) { return player; }
    public void append(Player player) { live_players.add(player); }
    public void clear_live_players() { live_players.clear(); }
    public static void collect() {
        // Ordinary Java allocation/collection from the native owner, outside
        // an entry callback. GC progress is verified through management beans.
        for (int i = 0; i < 16; ++i) { blackhole += new byte[32768].length; }
        System.gc();
    }
    public static long throughput_loop(int count) {
        long sum = 0;
        for (int i = 0; i < count; ++i) { sum += plain(i); }
        return sum;
    }
    public static long unrelated_loop(int count) {
        long sum = 0;
        for (int i = 0; i < count; ++i) { sum += ((i * 3) ^ 7); }
        return sum;
    }
    public static int plain(int value) { return value + 1; }
    public static int read_total() { return total; }
    public static void write_total(int value) { total = value; }
    public static void load_later() { blackhole += new Later().value; }

    public static final class Player {
        public int id;
        public Player(int value) { id = value; }
    }
    public static final class Later { public int value = 1; }

    public static void main(String[] arguments) throws Exception {
        // Warm ordinary Java bodies so examples also encounter compiled code.
        for (int i = 0; i < 200000; ++i) {
            blackhole += plain(i) + read_total();
            write_total(100);
        }
        for (int i = 0; i < 512; ++i) {
            blackhole += throughput_loop(4096) + unrelated_loop(4096);
        }
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        for (;;) { Thread.sleep(60000); }
    }
}

import java.util.Arrays;
import java.util.List;

public class ExampleFixture {
    public int value;
    public static int total = 100;
    public static final List<Player> players = Arrays.asList(new Player(7), new Player(11), null);
    private static volatile long blackhole;

    public ExampleFixture(int initial) { value = initial; }
    public int add(int amount) { return value += amount; }
    public static String thread_name() { return Thread.currentThread().getName(); }
    public static Player echo(Player player) { return player; }
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
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        Thread.sleep(300000);
    }
}

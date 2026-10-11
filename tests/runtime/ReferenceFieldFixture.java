import java.io.File;
import java.lang.management.ManagementFactory;

public final class ReferenceFieldFixture {
    public static final class Player {
        public final int id;
        public Player(int value) { id = value; }
    }
    public static volatile Player reference = new Player(11);
    public static volatile Player prevented = new Player(12);
    public static volatile int sink;
    public static synchronized Player read() { return reference; }
    public static synchronized void write(Player value) { reference = value; }
    public static void collect() { System.gc(); }
    public static Player actual() throws Exception {
        return (Player)ReferenceFieldFixture.class.getDeclaredField("reference").get(null);
    }
    private static void require(boolean value, String label) {
        if (!value) throw new AssertionError(label);
    }
    public static void main(String[] args) throws Exception {
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
        }
        System.out.println("REFERENCE_FIELDS=pass"); System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

public final class ExecutorFixture {
    public static final class Node {
        public int value;
        public Node child;
        public Node(int value) { this.value = value; child = this; }
        public int add(int amount) { return value + amount; }
    }
    public static Node retained = new Node(17);
    public static Node[] retained_nodes = {retained};
    public static Node create(int value) { return new Node(value); }
    public static String thread_name() { return Thread.currentThread().getName(); }
    public static void collect() { System.gc(); }
    public static void fail() { throw new IllegalStateException("expected executor exception"); }
    public static void main(String[] arguments) throws Exception {
        new Node(0);
        System.out.println("PID=" + java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        while (true) {
            byte[][] garbage = new byte[32][];
            for (int index = 0; index < garbage.length; ++index) { garbage[index] = new byte[32768]; }
            System.gc();
            Thread.sleep(20);
        }
    }
}

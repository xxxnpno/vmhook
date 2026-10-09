import java.lang.management.ManagementFactory;
public class ObservationCallbackFixture {
    public static volatile boolean start;
    public static volatile boolean stop;
    public static volatile int gcEpoch;
    public static volatile int validReturns;
    public static volatile int nestingDepth;
    public static class Node {
        public int value;
        public Node(int value) { this.value = value; }
    }
    public static Node target(Node original, int i) { return original; }
    public static int nested(Node original) { return original.value; }
    public static void main(String[] args) throws Exception {
        Node preload = new Node(-1); nested(preload);
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        Thread gc = new Thread(() -> { while (!stop) { System.gc(); gcEpoch++; } });
        gc.start();
        while (!start) Thread.sleep(2);
        for (int i = 1; i <= 24; i++) {
            Node value = target(new Node(i), i);
            if (value == null || value.value != i + 1000) throw new AssertionError("Return lost " + i);
            validReturns++;
        }
        stop = true;
        System.out.println("RETURNS=" + validReturns);
        while (true) Thread.sleep(100);
    }
}

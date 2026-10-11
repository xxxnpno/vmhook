import java.lang.management.ManagementFactory;
public class HeapConcurrentFixture {
    public static class Node {
        public final int value;
        public Node next;
        public Node(int value) { this.value = value; }
    }
    static Node[] graph = new Node[250000];
    static byte[][] retained = new byte[512][];
    public static volatile long allocations;
    public static void main(String[] arguments) throws Exception {
        for (int index = 0; index < graph.length; ++index) {
            graph[index] = new Node(index);
            if (index != 0) graph[index].next = graph[index - 1];
        }
        graph[0].next = graph[graph.length - 1];
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("MAX_HEAP=" + Runtime.getRuntime().maxMemory());
        System.out.println("READY"); System.out.flush();
        long iteration = 0;
        while (true) {
            int slot = (int)(iteration++ % retained.length);
            retained[slot] = new byte[2 * 1024 * 1024];
            retained[slot][0] = (byte)iteration;
            allocations = iteration;
            if ((iteration & 1023) == 0) System.out.println("ALLOCATIONS=" + iteration);
            if ((iteration & 31) == 0) Thread.sleep(1);
        }
    }
}

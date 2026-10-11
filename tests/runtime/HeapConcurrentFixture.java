import java.lang.management.ManagementFactory;
public class HeapConcurrentFixture {
    public static class Node {
        public final int value;
        public Node next;
        public Node(int value) { this.value = value; }
    }
    static Node[] graph = new Node[250000];
    static byte[][] retained;
    public static volatile long allocations;
    public static void main(String[] arguments) throws Exception {
        for (int index = 0; index < graph.length; ++index) {
            graph[index] = new Node(index);
            if (index != 0) graph[index].next = graph[index - 1];
        }
        graph[0].next = graph[graph.length - 1];
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("MAX_HEAP=" + Runtime.getRuntime().maxMemory());
        long max_heap = Runtime.getRuntime().maxMemory();
        com.sun.management.HotSpotDiagnosticMXBean diagnostic =
            ManagementFactory.getPlatformMXBean(com.sun.management.HotSpotDiagnosticMXBean.class);
        long region = Long.parseLong(diagnostic.getVMOption("G1HeapRegionSize").getValue());
        if (region < 1024 * 1024 || region > 32L * 1024 * 1024)
            throw new IllegalStateException("Unsupported G1 region geometry for the marking witness");
        // A region-plus-1KiB humongous array occupies two regions. Target 60%
        // occupied regions while retaining about 30% of max heap as actual bytes.
        int allocation_bytes = (int)region + 1024;
        long ring_count = Math.max(2, (max_heap / 5 * 3) / (region * 2));
        long retained_bytes = ring_count * allocation_bytes;
        long physical = ((com.sun.management.OperatingSystemMXBean)
            ManagementFactory.getOperatingSystemMXBean()).getTotalPhysicalMemorySize();
        long allocation_budget = Math.min(2L * 1024 * 1024 * 1024, physical / 4);
        if (ring_count > Integer.MAX_VALUE || retained_bytes > allocation_budget)
            throw new IllegalStateException("Marking witness needs " + retained_bytes +
                " retained bytes; host budget is " + allocation_budget + "; MAX_HEAP=" + max_heap);
        retained = new byte[(int)ring_count][];
        System.out.println("G1_REGION_BYTES=" + region);
        System.out.println("RETAINED_BYTES=" + retained_bytes);
        System.out.println("READY"); System.out.flush();
        long iteration = 0;
        while (true) {
            int slot = (int)(iteration++ % retained.length);
            retained[slot] = new byte[allocation_bytes];
            retained[slot][0] = (byte)iteration;
            allocations = iteration;
            if ((iteration & 1023) == 0) System.out.println("ALLOCATIONS=" + iteration);
            if ((iteration & 31) == 0) Thread.sleep(1);
        }
    }
}

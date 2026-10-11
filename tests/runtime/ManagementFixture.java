import java.lang.management.ManagementFactory;
import java.util.concurrent.CountDownLatch;

public final class ManagementFixture {
    private static final CountDownLatch release = new CountDownLatch(1);
    private static volatile int sink;

    private static void blockedLeaf(boolean block) throws InterruptedException {
        if (block) release.await();
        else sink++;
    }
    private static void compiledCaller(boolean block) throws InterruptedException { blockedLeaf(block); }
    public static void collect() { System.gc(); }

    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 300000; ++i) compiledCaller(false);
        Thread observed = new Thread(new Runnable() {
            public void run() {
                try { compiledCaller(true); }
                catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
            }
        }, "VMHook-observed-thread");
        observed.start();
        while (observed.getState() != Thread.State.WAITING) Thread.yield();
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

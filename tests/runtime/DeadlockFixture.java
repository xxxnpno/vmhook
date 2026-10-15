import java.lang.management.ManagementFactory;
import java.lang.management.ThreadMXBean;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.ReentrantLock;

public final class DeadlockFixture {
    private static final Object contention_lock = new Object();
    private static final CountDownLatch holder_ready = new CountDownLatch(1);
    private static final CountDownLatch keep_alive = new CountDownLatch(1);
    private static final Object monitor_left = new Object();
    private static final Object monitor_right = new Object();
    private static final ReentrantLock synchronizer_left = new ReentrantLock();
    private static final ReentrantLock synchronizer_right = new ReentrantLock();
    private static Thread holder, contender, monitor_a, monitor_b, synchronizer_a, synchronizer_b;
    private static Thread virtual_thread;

    private static void park() {
        try { keep_alive.await(); }
        catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new AssertionError(error);
        }
    }

    private static void await(CountDownLatch latch) {
        try {
            if (!latch.await(45, TimeUnit.SECONDS)) throw new AssertionError("fixture latch timed out");
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new AssertionError(error);
        }
    }

    private static Thread daemon(String name, Runnable work) {
        Thread thread = new Thread(work, name);
        thread.setDaemon(true);
        thread.start();
        return thread;
    }

    private static void await_state(Thread thread, Thread.State state) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(45);
        while (thread.getState() != state) {
            if (System.nanoTime() >= deadline) throw new AssertionError("fixture state timed out: " + thread.getName());
            Thread.yield();
        }
    }

    public static void start_monitor_cycle() {
        final CountDownLatch held = new CountDownLatch(2);
        final CountDownLatch entering = new CountDownLatch(2);
        monitor_a = daemon("VMHook-monitor-a", new Runnable() {
            public void run() {
                synchronized (monitor_left) {
                    held.countDown(); await(held); entering.countDown();
                    synchronized (monitor_right) { throw new AssertionError("monitor cycle escaped"); }
                }
            }
        });
        monitor_b = daemon("VMHook-monitor-b", new Runnable() {
            public void run() {
                synchronized (monitor_right) {
                    held.countDown(); await(held); entering.countDown();
                    synchronized (monitor_left) { throw new AssertionError("monitor cycle escaped"); }
                }
            }
        });
        await(entering);
        await_state(monitor_a, Thread.State.BLOCKED);
        await_state(monitor_b, Thread.State.BLOCKED);
    }

    public static void start_synchronizer_cycle() {
        final CountDownLatch held = new CountDownLatch(2);
        final CountDownLatch entering = new CountDownLatch(2);
        synchronizer_a = daemon("VMHook-synchronizer-a", new Runnable() {
            public void run() {
                synchronizer_left.lock(); held.countDown(); await(held); entering.countDown();
                synchronizer_right.lock();
                throw new AssertionError("synchronizer cycle escaped");
            }
        });
        synchronizer_b = daemon("VMHook-synchronizer-b", new Runnable() {
            public void run() {
                synchronizer_right.lock(); held.countDown(); await(held); entering.countDown();
                synchronizer_left.lock();
                throw new AssertionError("synchronizer cycle escaped");
            }
        });
        await(entering);
        await_state(synchronizer_a, Thread.State.WAITING);
        await_state(synchronizer_b, Thread.State.WAITING);
    }

    public static long contention_owner_id() { return holder.getId(); }
    public static int contention_hash() { return System.identityHashCode(contention_lock); }
    public static long monitor_a_id() { return monitor_a.getId(); }
    public static long monitor_b_id() { return monitor_b.getId(); }
    public static int monitor_left_hash() { return System.identityHashCode(monitor_left); }
    public static int monitor_right_hash() { return System.identityHashCode(monitor_right); }
    public static long synchronizer_a_id() { return synchronizer_a.getId(); }
    public static long synchronizer_b_id() { return synchronizer_b.getId(); }
    public static long virtual_id() { return virtual_thread == null ? 0 : virtual_thread.getId(); }

    // Deterministic management-response seams test unsupported scope and a
    // ThreadInfo disappearing between detection and copying. Real cycles above
    // always use the real platform ThreadMXBean through the public API.
    public static ThreadMXBean unsupported_bean() {
        return (ThreadMXBean) Proxy.newProxyInstance(ThreadMXBean.class.getClassLoader(),
            new Class<?>[] {ThreadMXBean.class}, new InvocationHandler() {
                public Object invoke(Object proxy, Method method, Object[] args) {
                    if (method.getName().equals("isSynchronizerUsageSupported")) return Boolean.FALSE;
                    throw new AssertionError("unsupported scope was silently downgraded");
                }
            });
    }

    public static ThreadMXBean raced_bean() {
        final long id = Thread.currentThread().getId();
        return (ThreadMXBean) Proxy.newProxyInstance(ThreadMXBean.class.getClassLoader(),
            new Class<?>[] {ThreadMXBean.class}, new InvocationHandler() {
                public Object invoke(Object proxy, Method method, Object[] args) {
                    String name = method.getName();
                    if (name.equals("isSynchronizerUsageSupported")) return Boolean.TRUE;
                    if (name.equals("findDeadlockedThreads")) return new long[] {id};
                    if (name.equals("getThreadInfo")) return null;
                    throw new AssertionError("unexpected raced-bean method: " + name);
                }
            });
    }

    public static void main(String[] args) throws Exception {
        holder = daemon("VMHook-contention-holder", new Runnable() {
            public void run() {
                synchronized (contention_lock) { holder_ready.countDown(); park(); }
            }
        });
        await(holder_ready);
        contender = daemon("VMHook-contention-contender", new Runnable() {
            public void run() {
                synchronized (contention_lock) { throw new AssertionError("contention holder escaped"); }
            }
        });
        await_state(contender, Thread.State.BLOCKED);
        try {
            Method start_virtual = Thread.class.getMethod("startVirtualThread", Runnable.class);
            virtual_thread = (Thread) start_virtual.invoke(null, new Runnable() {
                public void run() { park(); }
            });
            await_state(virtual_thread, Thread.State.WAITING);
        } catch (NoSuchMethodException older_java) {
            // The source remains Java 8 compatible; stable virtual threads start
            // only on Java 21+, without preview flags or a replacement JVM.
        }
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        // Every cycle is daemon-owned and deliberately remains deadlocked.
        // The harness terminates only this owned JVM after collecting results.
        for (;;) Thread.sleep(1000);
    }
}

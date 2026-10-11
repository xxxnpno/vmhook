import java.lang.management.ManagementFactory;
import java.lang.ref.WeakReference;

public class SpecialReferenceFixture {
    public static class Payload {
        public int value;
        public Payload(int value) { this.value = value; }
    }
    public static class Ordinary {
        public Object referent = new Payload(73);
    }
    public static class Shadowed extends WeakReference<Object> {
        public Object referent = new Payload(89);
        public Shadowed(Object value) { super(value); }
    }
    public static final Payload target = new Payload(41);
    public static final WeakReference<Object> weak = new WeakReference<Object>(target);
    public static final WeakReference<Object> cleared = new WeakReference<Object>(null);
    public static final Ordinary ordinary = new Ordinary();
    public static final Shadowed shadowed = new Shadowed(target);
    public static void main(String[] arguments) throws Exception {
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        while (true) { System.gc(); Thread.sleep(100); }
    }
}

import java.lang.management.ManagementFactory;

public class DirectArrayFixture {
    public static final DirectArrayFixture holder = new DirectArrayFixture();
    public final int[] integers = new int[32768];
    public final boolean[] booleans = new boolean[32768];
    public final char[] characters = new char[32768];
    public final String[] strings = new String[] { "unchanged" };
    public int[] absent;
    public static volatile long gc_epoch;
    public boolean verify(int expected, boolean flag) {
        for (int index=0;index<integers.length;++index)
            if (integers[index]!=expected || booleans[index]!=flag || characters[index]!=(char)0xE9) return false;
        return true;
    }
    public boolean string_unchanged() { return strings[0].equals("unchanged"); }
    public static long epoch() { return gc_epoch; }
    public static void main(String[] arguments) throws Exception {
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY"); System.out.flush();
        byte[][] pressure=new byte[16][];
        for (long iteration=0;;++iteration) {
            pressure[(int)(iteration%pressure.length)]=new byte[1024*1024];
            System.gc(); ++gc_epoch; Thread.sleep(1);
        }
    }
}

import java.lang.management.ManagementFactory;

// All of these are legal Java names. Their C++ spellings must preserve the
// binding without becoming a language token, constructor, or VMHook facade.
public final class GeneratedWrapperFixture {
    public int field = 11;
    public int instance = 12;
    public int nativeRoot = 13;
    public static int constructCalls;

    public int and_eq() { return 101; }
    public int or_eq() { return 102; }
    public int not_eq() { return 103; }
    public int xor_eq() { return 104; }
    public int generatedWrapperFixture() { return 105; }
    public int getInstance(int extra) { return 106 + extra; }
    public int getNativeRoot(int extra) { return 107 + extra; }
    public int setNativeRoot(int extra) { return 108 + extra; }
    public int getField(int extra) { return 109 + extra; }
    public int getMethod(int extra) { return 110 + extra; }
    public int staticMembers() { return 111; }
    public int self() { return 112; }
    public int is() { return 113; }
    public int javaContract() { return 114; }
    public int descriptor() { return 115; }
    public int construct() { constructCalls++; return 116; }
    public int snake__case() { return 117; }
    public int snake_case() { return 118; }
    public static String workerThreadName() { return Thread.currentThread().getName(); }
    public \uD801\uDC00Type generatedWrapperFixtureType() { return new \uD801\uDC00Type(); }

    public static final class ContainerTypes {
        public Iterable<Object> iterable;
        public java.util.Map.Entry<Object, Object> entry;
    }

    // U+10400 is a supplementary-plane Java identifier letter. The ASCII source
    // escapes allow the same fixture to compile under every host code page.
    public static class \uD801\uDC00Type {
        public int \uD801\uDC00Field = 211;
        public \uD801\uDC00Type \uD801\uDC00Reference;
        public \uD801\uDC00Type \uD801\uDC00Method(\uD801\uDC00Type value) {
            System.gc();
            return value;
        }
    }
    public static final class \uD801\uDC00TypeChild extends \uD801\uDC00Type {}
    public static int unicodeChoice(Object value) { return 1; }
    public static int unicodeChoice(\uD801\uDC00Type value) { return 2; }

    public static void main(String[] args) throws Exception {
        new \uD801\uDC00Type();
        new \uD801\uDC00TypeChild();
        new ContainerTypes();
        new registerJavaClasses();
        new std();
        new vmhook();
        new arg_0(1);
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");
        System.out.flush();
        for (;;) { Thread.sleep(100); }
    }
}

class registerJavaClasses {}
class std {}
class vmhook {}
class arg_0 { public arg_0(int value) {} }

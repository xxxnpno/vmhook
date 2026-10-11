import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.lang.management.ManagementFactory;
import java.lang.ref.PhantomReference;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.SoftReference;
import java.lang.ref.WeakReference;

public class ReferenceGraphFixture {
    public static class Node { public Object next; }
    public static class WeakOnly { }
    public static class Base { public Object inherited; }
    public static class Shadow extends WeakReference<Object> {
        public Object referent;
        public Shadow(Object weak, Object strong) { super(weak); referent=strong; }
    }
    public static class Copy { public Object next; public Copy() { next=this; } }
    public static class Root extends Base {
        public Node first;
        public Object[] objects;
        public WeakReference<Object> weak;
        public SoftReference<Object> soft;
        public PhantomReference<Object> phantom;
        public Shadow shadow;
        public Object copy_one;
        public Object copy_two;
    }
    public static final A short_name=new A();
    public static final Object weak_alive=new WeakOnly();
    public static final Root root=initialize();
    public static volatile long gc_epoch;
    static Object copy() throws Exception {
        final String name="ReferenceGraphFixture$Copy";
        InputStream input=ReferenceGraphFixture.class.getResourceAsStream("/"+name+".class");
        if(input==null) throw new IllegalStateException("Copy class bytes unavailable");
        ByteArrayOutputStream output=new ByteArrayOutputStream();
        byte[] chunk=new byte[4096];
        for(int count;(count=input.read(chunk))!=-1;) output.write(chunk,0,count);
        input.close(); final byte[] bytes=output.toByteArray();
        ClassLoader loader=new ClassLoader(ReferenceGraphFixture.class.getClassLoader()) {
            protected Class<?> loadClass(String requested,boolean resolve) throws ClassNotFoundException {
                if(!requested.equals(name)) return super.loadClass(requested,resolve);
                Class<?> found=findLoadedClass(requested);
                if(found==null) found=defineClass(requested,bytes,0,bytes.length);
                if(resolve) resolveClass(found);
                return found;
            }
        };
        return loader.loadClass(name).getConstructor().newInstance();
    }
    static Root initialize() {
        try {
            Root value=new Root(); Node a=new Node(), b=new Node(); a.next=b; b.next=a;
            value.first=a; value.inherited=b;
            value.objects=new Object[]{a,a,null,value,new int[]{1,2,3},new Object[]{b,a}};
            value.weak=new WeakReference<Object>(weak_alive);
            value.soft=new SoftReference<Object>(weak_alive);
            value.phantom=new PhantomReference<Object>(weak_alive,new ReferenceQueue<Object>());
            value.shadow=new Shadow(weak_alive,a);
            value.copy_one=copy();value.copy_two=copy();return value;
        } catch(Exception error) {throw new ExceptionInInitializerError(error);}
    }
    public static boolean weak_referent_alive() {return root.weak.get()==weak_alive && root.soft.get()==weak_alive;}
    public static long epoch() {return gc_epoch;}
    public static void main(String[] args) throws Exception {
        System.out.println("PID="+ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY");System.out.flush();
        byte[][] pressure=new byte[16][];
        for(long iteration=0;;++iteration) {
            pressure[(int)(iteration%pressure.length)]=new byte[1024*1024];
            System.gc();++gc_epoch;Thread.sleep(1);
        }
    }
}

class A {public Object self=this;}

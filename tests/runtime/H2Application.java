import java.lang.management.ManagementFactory;
import org.h2.tools.Server;

/** Application startup only: all SQL and validation run in the injected C++ DLL. */
public final class H2Application {
    public static Server server;

    public static void main(String[] arguments) throws Exception {
        server = Server.createTcpServer("-tcpPort", "0", "-ifNotExists").start();
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("PORT=" + server.getPort());
        System.out.println("READY");
        System.out.flush();
        for (;;) Thread.sleep(100);
    }
}

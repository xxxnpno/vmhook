import java.lang.management.ManagementFactory;
import java.util.*;

public final class InteropFixture {
    public static final class Player {
        public final int id;
        public Player(int next) { id = next; }
    }
    public static final class Wide {
        public final long total;
        public Wide(int a, long b, double c, Player d, int e, long f, double g, Player h, String i, int j) {
            requireWorker();
            System.gc();
            if (a != 1 || b != 0x1234567812345678L || c != 3.25 || d.id != 11 || e != 5 ||
                f != -9000000000L || g != -7.5 || h.id != 22 || !i.equals("ninth") || j != 10) {
                throw new AssertionError("Mixed constructor arguments changed");
            }
            total = 1234;
        }
    }
    static void requireWorker() {
        if (!Thread.currentThread().getName().startsWith("VMHook-interop-")) {
            throw new AssertionError("Call left the native worker");
        }
    }
    public static long mixed(int a, long b, double c, Player d, int e, long f, double g, Player h, String i, int j) {
        requireWorker();
        System.gc();
        if (a != 1 || b != 0x1234567812345678L || c != 3.25 || d.id != 11 || e != 5 ||
            f != -9000000000L || g != -7.5 || h.id != 22 || !i.equals("ninth") || j != 10) {
            throw new AssertionError("Mixed method arguments changed");
        }
        return 1234;
    }
    public static int manyReferences(
        Player arg_0, Player arg_1, Player arg_2, Player arg_3, Player arg_4, Player arg_5, Player arg_6, Player arg_7,
        Player arg_8, Player arg_9, Player arg_10, Player arg_11, Player arg_12, Player arg_13, Player arg_14, Player arg_15,
        Player arg_16, Player arg_17, Player arg_18, Player arg_19, Player arg_20, Player arg_21, Player arg_22, Player arg_23,
        Player arg_24, Player arg_25, Player arg_26, Player arg_27, Player arg_28, Player arg_29, Player arg_30, Player arg_31,
        Player arg_32, Player arg_33, Player arg_34, Player arg_35, Player arg_36, Player arg_37, Player arg_38, Player arg_39,
        Player arg_40, Player arg_41, Player arg_42, Player arg_43, Player arg_44, Player arg_45, Player arg_46, Player arg_47,
        Player arg_48, Player arg_49, Player arg_50, Player arg_51, Player arg_52, Player arg_53, Player arg_54, Player arg_55,
        Player arg_56, Player arg_57, Player arg_58, Player arg_59, Player arg_60, Player arg_61, Player arg_62, Player arg_63) {
        requireWorker();
        System.gc();
        Object[] values = {
            arg_0, arg_1, arg_2, arg_3, arg_4, arg_5, arg_6, arg_7, arg_8, arg_9, arg_10, arg_11,
            arg_12, arg_13, arg_14, arg_15, arg_16, arg_17, arg_18, arg_19, arg_20, arg_21, arg_22, arg_23,
            arg_24, arg_25, arg_26, arg_27, arg_28, arg_29, arg_30, arg_31, arg_32, arg_33, arg_34, arg_35,
            arg_36, arg_37, arg_38, arg_39, arg_40, arg_41, arg_42, arg_43, arg_44, arg_45, arg_46, arg_47,
            arg_48, arg_49, arg_50, arg_51, arg_52, arg_53, arg_54, arg_55, arg_56, arg_57, arg_58, arg_59,
            arg_60, arg_61, arg_62, arg_63
        };
        for (Object value : values) {
            if (value != arg_0 || ((Player)value).id != 11) {
                throw new AssertionError("Typed references changed across GC");
            }
        }
        return values.length;
    }
    public static int maximumReferences(
        Object arg_0, Object arg_1, Object arg_2, Object arg_3, Object arg_4,
        Object arg_5, Object arg_6, Object arg_7, Object arg_8, Object arg_9,
        Object arg_10, Object arg_11, Object arg_12, Object arg_13, Object arg_14,
        Object arg_15, Object arg_16, Object arg_17, Object arg_18, Object arg_19,
        Object arg_20, Object arg_21, Object arg_22, Object arg_23, Object arg_24,
        Object arg_25, Object arg_26, Object arg_27, Object arg_28, Object arg_29,
        Object arg_30, Object arg_31, Object arg_32, Object arg_33, Object arg_34,
        Object arg_35, Object arg_36, Object arg_37, Object arg_38, Object arg_39,
        Object arg_40, Object arg_41, Object arg_42, Object arg_43, Object arg_44,
        Object arg_45, Object arg_46, Object arg_47, Object arg_48, Object arg_49,
        Object arg_50, Object arg_51, Object arg_52, Object arg_53, Object arg_54,
        Object arg_55, Object arg_56, Object arg_57, Object arg_58, Object arg_59,
        Object arg_60, Object arg_61, Object arg_62, Object arg_63, Object arg_64,
        Object arg_65, Object arg_66, Object arg_67, Object arg_68, Object arg_69,
        Object arg_70, Object arg_71, Object arg_72, Object arg_73, Object arg_74,
        Object arg_75, Object arg_76, Object arg_77, Object arg_78, Object arg_79,
        Object arg_80, Object arg_81, Object arg_82, Object arg_83, Object arg_84,
        Object arg_85, Object arg_86, Object arg_87, Object arg_88, Object arg_89,
        Object arg_90, Object arg_91, Object arg_92, Object arg_93, Object arg_94,
        Object arg_95, Object arg_96, Object arg_97, Object arg_98, Object arg_99,
        Object arg_100, Object arg_101, Object arg_102, Object arg_103, Object arg_104,
        Object arg_105, Object arg_106, Object arg_107, Object arg_108, Object arg_109,
        Object arg_110, Object arg_111, Object arg_112, Object arg_113, Object arg_114,
        Object arg_115, Object arg_116, Object arg_117, Object arg_118, Object arg_119,
        Object arg_120, Object arg_121, Object arg_122, Object arg_123, Object arg_124,
        Object arg_125, Object arg_126, Object arg_127, Object arg_128, Object arg_129,
        Object arg_130, Object arg_131, Object arg_132, Object arg_133, Object arg_134,
        Object arg_135, Object arg_136, Object arg_137, Object arg_138, Object arg_139,
        Object arg_140, Object arg_141, Object arg_142, Object arg_143, Object arg_144,
        Object arg_145, Object arg_146, Object arg_147, Object arg_148, Object arg_149,
        Object arg_150, Object arg_151, Object arg_152, Object arg_153, Object arg_154,
        Object arg_155, Object arg_156, Object arg_157, Object arg_158, Object arg_159,
        Object arg_160, Object arg_161, Object arg_162, Object arg_163, Object arg_164,
        Object arg_165, Object arg_166, Object arg_167, Object arg_168, Object arg_169,
        Object arg_170, Object arg_171, Object arg_172, Object arg_173, Object arg_174,
        Object arg_175, Object arg_176, Object arg_177, Object arg_178, Object arg_179,
        Object arg_180, Object arg_181, Object arg_182, Object arg_183, Object arg_184,
        Object arg_185, Object arg_186, Object arg_187, Object arg_188, Object arg_189,
        Object arg_190, Object arg_191, Object arg_192, Object arg_193, Object arg_194,
        Object arg_195, Object arg_196, Object arg_197, Object arg_198, Object arg_199,
        Object arg_200, Object arg_201, Object arg_202, Object arg_203, Object arg_204,
        Object arg_205, Object arg_206, Object arg_207, Object arg_208, Object arg_209,
        Object arg_210, Object arg_211, Object arg_212, Object arg_213, Object arg_214,
        Object arg_215, Object arg_216, Object arg_217, Object arg_218, Object arg_219,
        Object arg_220, Object arg_221, Object arg_222, Object arg_223, Object arg_224,
        Object arg_225, Object arg_226, Object arg_227, Object arg_228, Object arg_229,
        Object arg_230, Object arg_231, Object arg_232, Object arg_233, Object arg_234,
        Object arg_235, Object arg_236, Object arg_237, Object arg_238, Object arg_239,
        Object arg_240, Object arg_241, Object arg_242, Object arg_243, Object arg_244,
        Object arg_245, Object arg_246, Object arg_247, Object arg_248, Object arg_249,
        Object arg_250, Object arg_251, Object arg_252, Object arg_253, Object arg_254) {
        requireWorker();
        System.gc();
        Object[] values = {
            arg_0, arg_1, arg_2, arg_3, arg_4, arg_5, arg_6, arg_7, arg_8, arg_9, arg_10, arg_11,
            arg_12, arg_13, arg_14, arg_15, arg_16, arg_17, arg_18, arg_19, arg_20, arg_21, arg_22, arg_23,
            arg_24, arg_25, arg_26, arg_27, arg_28, arg_29, arg_30, arg_31, arg_32, arg_33, arg_34, arg_35,
            arg_36, arg_37, arg_38, arg_39, arg_40, arg_41, arg_42, arg_43, arg_44, arg_45, arg_46, arg_47,
            arg_48, arg_49, arg_50, arg_51, arg_52, arg_53, arg_54, arg_55, arg_56, arg_57, arg_58, arg_59,
            arg_60, arg_61, arg_62, arg_63, arg_64, arg_65, arg_66, arg_67, arg_68, arg_69, arg_70, arg_71,
            arg_72, arg_73, arg_74, arg_75, arg_76, arg_77, arg_78, arg_79, arg_80, arg_81, arg_82, arg_83,
            arg_84, arg_85, arg_86, arg_87, arg_88, arg_89, arg_90, arg_91, arg_92, arg_93, arg_94, arg_95,
            arg_96, arg_97, arg_98, arg_99, arg_100, arg_101, arg_102, arg_103, arg_104, arg_105, arg_106, arg_107,
            arg_108, arg_109, arg_110, arg_111, arg_112, arg_113, arg_114, arg_115, arg_116, arg_117, arg_118, arg_119,
            arg_120, arg_121, arg_122, arg_123, arg_124, arg_125, arg_126, arg_127, arg_128, arg_129, arg_130, arg_131,
            arg_132, arg_133, arg_134, arg_135, arg_136, arg_137, arg_138, arg_139, arg_140, arg_141, arg_142, arg_143,
            arg_144, arg_145, arg_146, arg_147, arg_148, arg_149, arg_150, arg_151, arg_152, arg_153, arg_154, arg_155,
            arg_156, arg_157, arg_158, arg_159, arg_160, arg_161, arg_162, arg_163, arg_164, arg_165, arg_166, arg_167,
            arg_168, arg_169, arg_170, arg_171, arg_172, arg_173, arg_174, arg_175, arg_176, arg_177, arg_178, arg_179,
            arg_180, arg_181, arg_182, arg_183, arg_184, arg_185, arg_186, arg_187, arg_188, arg_189, arg_190, arg_191,
            arg_192, arg_193, arg_194, arg_195, arg_196, arg_197, arg_198, arg_199, arg_200, arg_201, arg_202, arg_203,
            arg_204, arg_205, arg_206, arg_207, arg_208, arg_209, arg_210, arg_211, arg_212, arg_213, arg_214, arg_215,
            arg_216, arg_217, arg_218, arg_219, arg_220, arg_221, arg_222, arg_223, arg_224, arg_225, arg_226, arg_227,
            arg_228, arg_229, arg_230, arg_231, arg_232, arg_233, arg_234, arg_235, arg_236, arg_237, arg_238, arg_239,
            arg_240, arg_241, arg_242, arg_243, arg_244, arg_245, arg_246, arg_247, arg_248, arg_249, arg_250, arg_251,
            arg_252, arg_253, arg_254
        };
        for (Object value : values) {
            if (value != arg_0 || ((Player)value).id != 11) {
                throw new AssertionError("Maximum parameter references changed across GC");
            }
        }
        return values.length;
    }
    public static Throwable throwable() { requireWorker(); return new IllegalStateException("interop-error"); }
    public static String surrogate() { requireWorker(); return "\ud800X"; }
    public static void collect() { requireWorker(); System.gc(); }
    public static void verifyPrimitive(Object value, String descriptor, int length) {
        requireWorker();
        if (!value.getClass().getName().equals(descriptor) || java.lang.reflect.Array.getLength(value) != length) {
            throw new AssertionError("Array class or length changed");
        }
    }
    public static Player[] objectArray(Player first, Player last) {
        requireWorker();
        return new Player[] { first, null, last };
    }
    public static void main(String[] args) throws Exception {
        // Ensure only application classes are loaded; Array and unused primitive
        // wrappers may be loaded by VMHook through bootstrap Class.forName.
        new Player(0);
        Class.forName("InteropFixture$Wide", false, InteropFixture.class.getClassLoader());
        new ArrayList<Object>(); new HashMap<Object,Object>(); new HashSet<Object>(); new ArrayDeque<Object>();
        System.out.println("PID=" + ManagementFactory.getRuntimeMXBean().getName().split("@")[0]);
        System.out.println("READY"); System.out.flush();
        for (;;) { Thread.sleep(100); }
    }
}

# Contributing

VMHook has two public interfaces for the same Windows x64 HotSpot runtime:
a C++23 header and a C++26 named module. G++ with the MinGW-w64 Windows SDK is
the supported compiler. The header stays C++23; the module uses current C++
features, including static reflection.

## Required behavior

- Never call JNI or JVMTI. Avoiding their headers while calling function-table
  slots, exported JNI entry points, or reflection methods through JNI still
  violates this rule.
- Work inside an already-running JVM without requiring launch arguments or
  replacement bytecode. Hook installation may temporarily pause compiler
  dispatch and deoptimize existing code through real VM safepoint operations;
  it must restore the previous compiler setting and let unrelated methods
  compile afterward. Do not permanently disable the VM's JIT.
- Account for already-compiled methods, inlined callers and compilation tasks
  started before installation. An interpreter-only assumption, a null
  `Method::_code` write or an unverified dependency scan is insufficient.
- Static field hooks intercept Java `getstatic`/`putstatic` through ASM stubs
  before access. Preserve replacement, cancellation and volatile semantics;
  do not substitute polling or post-write debug-register notifications.
  Account for compiler intrinsics, special interpreter entries and class
  definitions overlapping installation; compilation flags alone are insufficient.
  Class linking may replace `Method` metadata. Resolve the current method from
  its declaring class and stable metadata identity before applying or restoring
  policy; a rooted class mirror does not keep every discarded method copy alive.
- Class notifications follow successful definitions and expose the actual
  defining loader identity. Observer callbacks cannot inject, rewrite or veto
  class definitions. If field-policy setup fails, report a rooted Java exception
  before exposing an unprotected accessor, without allocating in that failure path.
- Keep static and instance wrappers simple: `get_method` and `get_field` are
  inherited instance functions that bind the current CRTP wrapper without an
  explicit receiver argument. Class-bound lookup uses the same names under
  `static_members`; a static wrapper function uses that qualifier explicitly.
- Hooks persist until explicitly removed. Do not add scoped hook handles.
- Keep preprocessor directives out of every `.ixx` file. Import the standard
  library and use the SDK header unit for Windows declarations.
- Preserve C++23/module behavior parity. Reflection can improve compile-time
  metadata and diagnostics; it must not make the header require C++26.

## Making a change

Edit `modules/vmhook.ixx`, then regenerate `include/vmhook/vmhook.hpp` with
`python tools/make_header.py`. Run `python tools/make_header.py --check` before
submitting. Update the explicit reflection fallbacks in the generator when a
new C++26 construct needs a C++23 equivalent; do not hand-edit the generated
header.

Read the current implementation before trusting a comment or an old changelog
entry. Validate internal layouts and calling conventions against the relevant
[OpenJDK HotSpot source](https://github.com/openjdk/jdk/tree/master/src/hotspot)
and the [Windows x64 ABI](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention).
Record the exact source revision when it justifies a layout or instruction
sequence. An observed offset or prologue on one JDK is not a portable guarantee.

Keep comments and documentation in English. Use snake_case for C++ identifiers,
including wrapper types and public APIs. Preserve exact Java class/member
strings and external Windows/HotSpot ABI names. Match the surrounding
formatting, use descriptive names, and prefer trailing return types. Use `constexpr`,
`consteval`, and `noexcept` where their actual contracts permit them. Runtime
metadata and VM state still require runtime validation.

Keep changes focused. Do not commit binaries, build directories, module caches,
precompiled headers, downloaded toolchains, or crash dumps. Do not add build
branches for other operating systems or compilers.

## Validation

Use the build commands in [README.md](README.md). Check both public interfaces
when changing shared behavior and keep their wrapper examples compilable.

For low-level changes, add a focused regression that would fail with the old
behavior. Separate compilation checks from tests in a live JVM. Report the
compiler, JDK build, collector, and execution environment with the result.
A cross-compiled executable or a declaration-only smoke test does not validate
injection, moving GC, native-thread dispatch, or a live interpreter hook.

Use [the observation fixtures](docs/hooks.md#reproduce-the-public-observation-fixtures)
for JIT, field and class changes. Prove actual compilation and inlining before
installing a method hook, exercise newly compiled callers and newly loaded
field accessors, and verify compiler settings and original bodies after
removal. Include callback reentry, moving GC, all primitive widths, failed
class definitions and same-name classes from different defining loaders where
those behaviors change.

Check failure paths as well as successful calls: a missing member, a null
receiver, an unsupported layout, a Java exception, and a dispatch task that
cannot run must have an observable result. Never turn unavailable invocation
into a successful zero result or a constructor-less object.

Tests must launch their JVM normally. Do not make them pass by adding `-Xint`,
turning off the JIT or collector, or linking JNI into VMHook.

## Pull requests

Describe the concrete behavior that changes and include the checks actually
run. State live JVM limitations precisely. Avoid benchmark figures without a
reproducible workload, toolchain, and environment. Use the private reporting
process in [SECURITY.md](SECURITY.md) for memory safety vulnerabilities.

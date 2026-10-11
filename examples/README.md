# Standalone DLL examples

These four small payloads use the same wrapper API as an application. Each
starts a native worker; `DllMain` only starts that worker and returns. Java
operations run after loader initialization, outside the loader lock. Keep the
DLL loaded for the JVM lifetime, including after hook removal.

| Payload | Demonstrates |
| --- | --- |
| `native_worker.cpp` | Context on the DLL's own OS thread, real Java constructor, instance call and static field |
| `wrappers_collections.cpp` | Short CRTP wrappers, inferred `to_vector`, object arguments and allocated Java-null wrappers |
| `method_hook.cpp` | Persistent method hook, forced return, original body and explicit shutdown |
| `field_class_observer.cpp` | Static primitive access stubs, replacement, write prevention and successful class-definition notification |

`ExampleFixture.java` is an ordinary Java application. It warms method/field
accessors before accepting a payload. Hooks therefore encounter normal JIT
execution. The full observation fixtures provide stronger explicit compilation
and inlining checks; these examples are a first working payload.

## Build from an installed package

After [installing VMHook](../docs/packaging.md), build the header examples:

```sh
cmake -S examples -B build/examples -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_PREFIX_PATH=C:/libraries/vmhook -DCMAKE_BUILD_TYPE=Release
cmake --build build/examples
```

For module examples add `-DVMHOOK_EXAMPLES_MODULE=ON` and use the required
G++ 16/CMake toolchain. The installed module is rebuilt in this example
project. A repository build can enable the examples with
`-DVMHOOK_BUILD_EXAMPLES=ON`; select module examples explicitly with
`-DVMHOOK_EXAMPLES_MODULE=ON`.

## Run the examples

The harness launches its own Java process and injects only that process. Build
the repository's injector with `-DVMHOOK_BUILD_RUNTIME_TESTS=ON`, then run on
native Windows with a supported exact JVM image:

```sh
python tools/run_examples.py --java C:/jdk/bin/java.exe --javac C:/jdk/bin/javac.exe --injector build/tests/vmhook_test_injector.exe --binaries build/examples --evidence build/examples.json
```

The Java command contains only its classpath and main class. No VM tuning,
agent or JIT arguments are required. The harness reports each asserted result,
keeps the DLL hashes and Java launcher identity in its evidence file, and
closes the fixture afterward. `--example native_worker` selects one payload.
`--emulator` is an explicit development option; emulator results are recorded
separately from native Windows evidence.

## Reference semantics

Java objects use `std::unique_ptr<example::player>`. The unique pointer owns a
C++ wrapper and its VMHook reference; Java manages the Java object. A Java
null has a non-empty C++ owner whose `get_instance()` is null. Object arguments
use these owners by const reference. Context-bound wrappers stay on their
owning thread and are destroyed before that context closes.

Instance wrappers call `get_field("id")` and `get_method("add", "(I)I")`
without a receiver argument. Static wrappers use the inherited
`static_members` qualifier internally; their callers write
`example::fixture::get_total()`. Snake_case applies to C++ names; strings keep
the exact Java names/descriptors.

The field example covers `getstatic`/`putstatic` bytecodes. A raw VM field read,
reflection, Unsafe or a folded constant is a different access path. The class
observer copies only a count during the callback, then calls Java on the
worker after the callback has returned. It does not retain borrowed metadata,
block class definition, or inject a class.

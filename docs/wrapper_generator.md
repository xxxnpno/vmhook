# Optional wrapper generation

Handwritten wrappers remain the default. `tools/generate_wrappers.py` can also
read Java `.class` files or a JAR and generate the same owning C++ API without
starting a JVM. It requires Python 3.10 or later and has no third-party packages.

```sh
python tools/generate_wrappers.py application.jar \
    --class example/Player --class example/Game \
    --namespace application --output generated/application.hpp
```

The header uses C++23 and `<vmhook/vmhook.hpp>`. For a C++26 interface, add
`--module application.wrappers --output generated/application.ixx`. That output
imports `std` and `vmhook`, contains no preprocessor directives, and annotates
each class with `vmhook::java_class` for compile-time reflection.

Generate only the wrappers you need with repeated `--class` options. Without
them, all base-version classes in the input are included; version-specific
`META-INF/versions/` JAR entries are ignored. Multiple input paths are accepted.
Duplicate class definitions and missing requested classes are explicit errors.

At runtime, call the generated namespace's `register_java_classes()` after the
classes are loaded, then establish a `vmhook::native_thread_context` before
calling Java. Registration identifies the loaded classes; the generator does
not load application classes, inject the DLL, or change JVM launch options.

Public methods, constructors, and fields become snake_case methods. Static
members use `static_members::get_method` or `static_members::get_field` inside
the wrapper; callers use the generated class's static method normally.
Descriptors are pinned for method calls and constructor factories. Final fields
have getters, and synthetic and bridge members are omitted.

Java references are `std::unique_ptr` results or `const std::unique_ptr<...>&`
arguments. Java null retains an allocated wrapper whose `get_instance()` is
null. Arrays use `vmhook::array<T>`, including nested arrays and all eight
primitive types. Common Java library types map to VMHook's built-in wrappers;
unselected reference classes map to `vmhook::any_object`.

Generic types remain erased at runtime. `--schema-json schema.json` preserves
the original `Signature` attributes for inspection, but the generator does not
infer a collection's element type from a signature. A collection can still use
the normal `to_vector()` conversion in a typed C++ return statement. Overloads
that collapse to the same C++ parameter types receive a stable hash suffix.

The parser checks descriptor syntax, array dimensions, parameter slots, class
file bounds, and modified UTF-8. It does not replace the JVM's bytecode verifier.
Regenerate when the Java schema changes and compile the generated output with
the corresponding VMHook header or module before distributing it.

```sh
python tests/wrapper_generator.py
```

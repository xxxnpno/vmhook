# Real H2 TCP application validation

VMHook's optional H2 workload starts an ordinary H2 TCP database server, then
injects the owned DLL. A DLL-created Windows OS thread opens two real TCP JDBC
connections and performs all SQL through VMHook. The Java launcher only starts
the server and announces its PID/port; it does not construct rows, run queries,
implement transactions or validate results for C++.

The application dependency is **H2 2.2.224** (`com.h2database:h2:2.2.224`),
downloaded from Maven Central. Its publisher's SHA256 is pinned in
[the dependency record](../tests/dependencies/h2-2.2.224.json):

```text
b9d8f19358ada82a4f6eb5b174c6cfe320a375b5a9cb5a4fe456d623e6e55497
```

The artifact contains 1,049 ordinary class entries at Java class version 52
(Java 8). Its three newer classes are correctly confined to MultiRelease
entries for Java 9, 10 and 21. The runner checks the artifact hash, size, class
versions and manifest before startup. The source JAR, publisher POM and tagged
license were independently audited; their URLs/hashes are in the dependency
record. H2 is dual licensed under **MPL-2.0 OR EPL-1.0** by H2 Group. No H2 JAR
or compiled dependency is committed or included in VMHook's source package.

## Build and run

Build the owned injector and both application DLLs in a normal Windows GNU
configuration:

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON -DVMHOOK_BUILD_RUNTIME_TESTS=ON -DVMHOOK_BUILD_MODULE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target vmhook_test_injector vmhook_h2_application_probe vmhook_module_h2_application_probe
python tools/run_h2_application.py --java C:/jdk/bin/java.exe --javac C:/jdk/bin/javac.exe --injector build/tests/vmhook_test_injector.exe --probe build/tests/vmhook_h2_application_probe.dll --transactions 32 --evidence build/h2-header.json
python tools/run_h2_application.py --java C:/jdk/bin/java.exe --javac C:/jdk/bin/javac.exe --injector build/tests/vmhook_test_injector.exe --probe build/tests/vmhook_module_h2_application_probe.dll --transactions 32 --evidence build/h2-module.json
```

Without `--jar`, the runner downloads and verifies the pinned artifact into
`<system temporary directory>/vmhook-h2-dependencies`. Use `--cache <directory>`
to choose a reusable download location, or `--jar <h2-2.2.224.jar>` for an offline
copy. A supplied/cached artifact with the wrong hash fails explicitly. Build
and package creation do not download H2. The runner preserves failure evidence
when the dependency, application compilation, startup, injection or workload
fails. The default workload deadline is 300 seconds; `--timeout` changes it.

Each run uses a fresh JVM and an ephemeral local TCP port. The server's normal
`-ifNotExists` service option permits this test's named in-memory database.
The JVM receives only its ordinary classpath and application main class. No
JVM tuning, agents, JNI/JVMTI calls or hook-driven dispatch queues are used by
the workload. The runner terminates its owned application at completion.

## Validated database work

The C++ wrappers use CRTP, `get_method`/`get_field` and owning Java wrappers.
For example, the JDBC connection wrapper's implementation stays small:

```cpp
auto prepare(std::string_view sql) const -> std::unique_ptr<prepared_statement>
{
    return get_method("prepareStatement", "(Ljava/lang/String;)Ljava/sql/PreparedStatement;")->call(sql);
}
```

The workload checks:

- Two independent TCP sessions, real JDBC metadata URLs and `H2VERSION()`.
- Prepared insertion of two accounts, supplementary Unicode, SQL nulls and
  binary values containing zero and negative byte values.
- Allocated C++ wrappers for Java-null driver results and SQL-null Strings.
- Visibility of committed changes through a second JDBC connection, isolation
  of an uncommitted update and restoration after rollback.
- Duplicate-key and syntax-error Java Throwables with exact SQLState/error
  codes, preserved across collection, followed by successful queries.
- Repeated transfers with debit, credit and audit insert in one committed
  transaction; checked account balances, audit cardinality and conserved total.
- Live Connection, PreparedStatement, ResultSet, String and byte-array owners
  across requested moving GC, observed through actual collector counts.
- Explicit closure of statements/results/connections after final validation.

The worker verifies Java's current thread name and its unchanged Windows
thread ID. Calls execute synchronously from that registered native worker.

## Measurements and evidence

`--transactions` accepts 1–10,000 transfers (default 32). Evidence records the
first context/two-connection durations, each raw transfer sample and its
nearest-rank p50/p99/min/max, total
workload wall time, transferred amount, Java heap observations and real GC
collection-count delta. Each transfer sample includes prepared-parameter
calls, three SQL operations, commit, and independent-session balance queries
with result checking and closure. Requested GC runs between samples; total
workload wall time includes those requests. This is a measured database
application workload with TCP, Java and VMHook costs together, not an isolated
bridge-overhead measurement or a comparison with another bridge.

The JSON also records source/pin/JAR/DLL/injector/Java/JVM hashes, actual launch
arguments, application output, exact check markers, dependency class audit and
fatal JVM reports. Preserve the raw evidence and module compiler manifest.
Repeat fresh processes on the same host for performance comparisons. There
are no timing pass/fail thresholds. `--wine <runner>` enables supplemental
Windows-binary checks on Linux; native Windows evidence is required for
published performance conclusions.

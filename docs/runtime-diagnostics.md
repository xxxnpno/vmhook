# Runtime diagnostics

VMHook resolves an exact Windows x64 HotSpot image. A Java major version is not
an ABI identifier: a vendor rebuild or security update can change private
symbols and layouts. The shipped manifest pins one image for every Java major
from 8 through 27, including the publisher's Temurin 27+35 release.

Inspect an image before injection:

```sh
python tools/inspect_runtime.py --dll path/to/jvm.dll
python tools/inspect_runtime.py --dll path/to/jvm.dll --pdb path/to/jvm.pdb
python tools/inspect_runtime.py --list
```

The JSON identifies the DLL by SHA-256, CodeView GUID and age, PE timestamp,
image size and architecture. Exit status 2 means the image has no shipped exact
profile. A supplied PDB must match the DLL. Identity matching alone does not
validate its private type layouts; use `private_profiles.py` and
`build_private_profiles.py` for that validation and reproducible generation.
These commands inspect files without executing them.

Inside the target JVM, query a passive preflight before preparing operations:

```cpp
const auto report = vmhook::runtime_capabilities();
const auto calls = report.support(vmhook::runtime_feature::native_calls);
if (calls.state == vmhook::feature_state::unavailable) {
    // Display calls.reason and the exact image identity to the user.
}
```

The report distinguishes an available backend, a conditional backend requiring
target-specific preparation, and an unavailable backend. It reports the actual
collector and compressed/compact layout flags when they can be read. Querying
it does not attach a native thread, start the persistent-root keeper, or install
a hook. A successful preflight does not replace the operation's checks: an
instance reference still needs its live owning context, and a hook still needs
its signature, root and JIT preparation.

An entry callback or forced return is not an event for an original method's
exit. The instance snapshot backend uses the verified G1/Parallel object iterator at
a VM safepoint and roots selected objects before Java resumes. Its capability
still requires a standalone native context and successful snapshot preparation.
Java monitor guards and static primitive/reference field hooks have separate
capability entries and operation-specific checks. Mounted virtual-thread
callbacks are skipped before entering the borrowed VM context; this preserves
the original Java operation and does not provide virtual-thread hook support.
The report keeps mixed-stack sampling and virtual-thread transitions unavailable
until their distinct backends are verified.

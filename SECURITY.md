# Security policy

VMHook accesses HotSpot metadata and executable memory inside a Windows x64
process. Memory safety errors in the library should be reported privately.

## Reporting a vulnerability

Open a [private security advisory](https://github.com/xxxnpno/vmhook/security/advisories/new)
with a minimal reproducer, the VMHook revision, the G++ version, and the exact
JDK build. Include the collector and any non-default JVM options when relevant,
plus a crash log or stack trace with private application data removed.

Report ordinary compatibility issues through GitHub issues. If an issue exposes
memory corruption or sensitive process data, use a private advisory instead.

## Supported versions

Fixes target the current default branch. There are no maintained LTS branches;
backport requests can be included in an advisory.

## Examples

- Out-of-bounds access, use-after-free, stale object references, or invalid
  generated machine code caused by VMHook.
- Incorrect use of Windows memory protection or exception handling.
- Thread, collector, or object-lifetime mistakes that corrupt JVM state.

An unsupported JDK or collector can still reveal a reportable library bug;
provide the reproducer so the failure can be distinguished from a refused
operation.

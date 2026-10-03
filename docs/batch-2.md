# Compatibility, status, and imported bundle handling

The status machine never equates a parsed Mach-O with a running game. `ANALYZED` means
no link happened; `PARTIALLY_RELINKED` means strong imports still lead to BRK traps;
`RELINKED` means binding succeeded but a needed subsystem is unsupported;
`RUNTIME_PARTIAL` indicates an incomplete required subsystem. `RUNTIME_READY` is
reserved for images whose *needed* subsystems are implemented, not merely parsed.
Unsupported UIKit/GLES/etc. evidence prevents that state. Encryption blocks linking.

The C++ shim runs process-wide exit handlers in reverse registration order, once each,
and releases its lock while invoking them. `__cxa_thread_atexit_impl` is currently
process-wide (not thread-local). C++ exceptions/unwinding are deliberately unresolved
and trap by name; Objective-C dispatch, Foundation, graphics, audio and input remain
unsupported. Guest memory execution requires ARM64 and has not been verified on a device.

IPA inspection is local and offline. PNG decode validates CRC, output limits and filters,
and converts Apple's CgBI channel order and alpha to standard RGBA. Icons are chosen
from declared plist entries, then loose icons, then CgBI records in Assets.car; absent
icons produce warnings, not substitutions. The import flow uses Java's ZipFile for IPA
container extraction; the C++ inflate decoder handles PNG/zlib data without linking zlib.
Java/NDK build and on-device execution are not host-verified.

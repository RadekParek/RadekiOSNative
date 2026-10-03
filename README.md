# RadekiOSNative (early foundation)
Native-first iOS -> Android ARM64 compatibility platform. Original AArch64 code is relinked, not emulated.

## What works (tested on host, 486 checks, ASan/UBSan clean)
- Mach-O: thin/fat, 32/64-bit, load commands, symbols, dyld-info rebase/bind, chained fixups (PTR_64, PTR_64_OFFSET, arm64e formats parsed), function starts, data-in-code, encryption info; hostile input rejected (mutation fuzz test).
- Analyzer: functions, call/branch/ADRP xrefs, PAC instruction counts, framework/ObjC/Swift/Metal/GLES hints, static blockers.
- Objective-C metadata (read-only): class objects -> `class_ro_t`, instance and class methods (both the classic absolute-pointer and the relative 12-byte encodings), ivars, properties, categories, protocols. It never throws on hostile metadata: anything it cannot verify is reported as a warning and flips `complete` to false. This is metadata reading, **not** a runtime: nothing is registered, dispatched or called.
- ARM64 relinker: slide to new base, rebase, bind to compat registry, unresolved imports become named BRK trap stubs by default (never fake success) or -- with a `StubFactory` -- logging no-op dispatch stubs (still reported unresolved), B/BL veneers, reference validator.
- Host runtime layer: libSystem subset + written libc++abi shims + libc++.1.dylib forwarding (explicit `std::string` and `__next_prime` wrappers under Apple's exact mangled names, plus a dlsym pass-through onto the host libc++) + partial framework shims (`UIApplicationMain`, `NSLog`, the objc memory family, ~80 dummy `_OBJC_CLASS_$_` objects for UIKit/EAGL/Foundation). UIKit/Foundation remain unsupported as full runtimes; the shims do not provide a game UI or event loop. See `docs/stubs-and-cxx-forwarding.md`.
- Validation: every PC-relative reference is checked against the mapped image. It walks real instruction ranges (sections marked `S_ATTR_*_INSTRUCTIONS`, plus trap stubs and veneers), so Mach-O headers, load commands and string constants are not mistaken for code.
- Loader (`loader/dyld.*`): several images in one address space -- dependency-first link order, per-image load bases and slides, cross-image binding (two-level, re-export chains, flat-namespace), per-image unresolved-import reporting. Two passes: the first fixes the layout, the second binds against the complete export table.
- Runtime: maps a relinked image (or a whole linked image set) into this process, W^X, signal-safe call into guest code, and a libSystem subset (stdio/string/malloc/libm, `_exit`, `_abort`, a static non-recursive pthread subset).
- Verification: a test-only mini AArch64 interpreter runs a relinked synthetic image and checks output; cross-image binding is verified structurally (bound pointers land inside the provider image, no trap stubs left).

## Not done (see progress.json / capabilities.json)
Execution on a device (the runtime has never run on real ARM64 hardware -- only compiled here), ARM64e PAC, ARMv7 AOT, an Objective-C runtime (message dispatch, class registration, `NSObject`), Foundation/UIKit, CoreFoundation/CoreGraphics/QuartzCore, Metal/GLES/Vulkan, audio, input, networking, filesystem.
The loader has no dyld shared cache, no `@rpath`/`@executable_path` expansion against a filesystem, no cross-image initializer ordering and no weak-symbol coalescing. Encrypted (FairPlay) binaries are refused; the tool does not decrypt anything.

## Build
`make` / `make test` / `make SAN=1 test` (host), or CMake (`CMakeLists.txt`), or the Android app under `android/`.

CLI:
```
radeki analyze  <macho> [--json] [--objc]
radeki symbols  <macho> [--json]
radeki ipa      <App.app> [-o dir] [--json]
radeki convert  <macho> [-o dir] [--load-base 0x..] [--json]
radeki validate <macho> [--load-base 0x..] [--json]
radeki load     <exe> [--dylib <path>]... [--load-base 0x..] [--json] [--stubs]
radeki run      <exe> [--dylib <path>]... [--load-base 0x..] [--json] [--traps]   # needs an ARM64 host
```
`run` uses the host runtime layer with dispatch stubs by default (so unbound imports log and
return safe defaults instead of SIGTRAP); `--traps` restores honest BRK trap stubs. `load` and
`symbols` accept `--stubs` to preview that mode. Exit codes: 0 ok, 1 error, 2 usage,
3 = linked, but some imports had no provider (trap or dispatch stubs remain).

The changelog lives next to `RadekiOSNative.zip` in the repository root.

## Batch 2 compatibility and IPA metadata
The 21-row capability matrix distinguishes static analysis, linking and actual runtime readiness.
UIKit, Foundation, Objective-C dispatch, graphics, audio, input, networking, Swift, PAC and ARMv7
remain unsupported or partial. A parsed Mach-O or even a fully bound image is **not** launchable evidence.
`radeki analyze` reports the measured state and needed capability rows; `radeki symbols` reports
per-slot classification. `radeki ipa` inspects an extracted `.app`, reads its Info.plist, and
converts an icon **from that bundle** to standard `icon.png`. No downloading or guessed icons.
The Android library stores imported bundles and native JSON analysis; its launcher only attempts
execution on ARM64 hardware, with no promise of game compatibility. The Settings screen exposes
real ARM64 compatibility-fallback and sampled missing-API tracing controls; ARM32 is explicitly
marked unavailable because no ARM32 runtime is packaged. Repository CI builds the arm64-v8a APK,
but execution on real ARM64 hardware remains unverified.

## Batch 3: libc++ forwarding, framework stubs and dispatch stubs
Targets the Minecraft PE 0.10.4 halt in `__GLOBAL__I_a17` (SIGTRAP on unbound libc++/framework
imports). The dyld relocation pass now maps Apple's mangled C++ symbols onto the host C++
runtime (explicit `basic_string` allocation/initialization wrappers speaking Apple's ABI, plus
a dlsym pass-through that finds the identical `std::__1` symbols in the NDK libc++ on device),
and the bind phase can replace BRK/SIGTRAP trap stubs with a generic dynamic stub dispatcher:
per-symbol AArch64 trampolines that log the missing symbol and return safe defaults
(0/NULL/nil, YES/1, x0 pass-through for the objc retain family, empty SEL), while data imports
bind to dummy class objects for the essential UIKit/EAGL/Foundation initializers. Honesty is
unchanged: stubbed imports stay listed as unresolved (`NOOP_STUB`), the state machine does not
move, and the default link mode keeps named BRK traps. Device execution remains unverified.

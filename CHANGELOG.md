# Changelog

All notable changes to RadekiOSNative. The project source ships as `RadekiOSNative.zip`; this
file is the channel log for that artifact and lives beside it in the repository root.

## 2026-10-03 - Android CLI link fix (missing liblog)

Host suite: **487 checks, 0 failures**, clean under `make SAN=1 test` (ASan + UBSan).

- Fixed the failing `android-arm64` CI job. The root `CMakeLists.txt` built the `radeki` library
  and CLI without linking `liblog`, so the `__android_log_write` call added to
  `runtime/compat_libsystem.cpp` (guest output on the device) left the Android link with an
  undefined symbol. The library now links `log` on Android (`PUBLIC`, so the CLI inherits it),
  which is what `android/app/src/main/cpp/CMakeLists.txt` has always done for the JNI library.
- Reproduced the failure and the fix at source level: with the sources compiled for
  `__ANDROID__` and `liblog` absent, the link fails on exactly `__android_log_write`, and it
  succeeds once `liblog` is provided. No C++ source changed, so host builds and the 487-check
  suite are unaffected. The NDK toolchain is not installed in the environment where this fix was
  prepared, so the real arm64-v8a cross-build is confirmed by the CI job itself.

## 2026-10-03 - libc++ hash-table crash fix, launch-path overhead and UIKit status clarity

Host suite: **486 checks, 0 failures**, clean under `make SAN=1 test` (ASan + UBSan).
The Android NDK/APK build and real-device Minecraft execution could not be verified here.

- Implemented the exact libc++ import `__ZNSt3__112__next_primeEm` reported as a no-op stub
  immediately before a SIGSEGV in `std::__1::__hash_table<ChunkPos>::__insert_unique`. The
  wrapper preserves libc++'s zero/empty-table case and otherwise returns the smallest prime at
  least the requested bucket count, with deterministic 64-bit Miller-Rabin checks and overflow
  protection. Added tests for symbol binding, edge values, a range of bucket counts, and a
  near-`SIZE_MAX` prime.
- Reduced compatibility-call tracing overhead: literal wrapper names now bypass temporary
  `std::string` creation, recent-call history uses a fixed 32-entry ring rather than a deque of
  heap-backed strings, and stub fallback launch no longer constructs an unused second registry.
- Kept the capability report honest and more specific: UIKit remains **Unsupported**. The
  existing startup shims are not a UIKit runtime and do not provide the application event loop,
  view/rendering pipeline or Android surface integration required to show a boot screen. The
  Settings description, compatibility summary and the `UIApplicationMain` placeholder output now
  say so rather than implying fallback mode is a complete game compatibility layer.

The reported `__next_prime` fault path is addressed in source, but further runtime failures may
follow. It must be retested with the rebuilt APK; no claim is made that Minecraft now reaches its
boot screen.

## 2026-10-03 - Android APK build fix and architecture-aware settings

Host suite: **476 checks, 0 failures**, clean under `make SAN=1 test` (ASan + UBSan).

- Fixed the ARM64 NDK compile failure: Android Bionic defines `st_atime_nsec`, `st_mtime_nsec`,
  and `st_ctime_nsec` as macros. The Darwin `stat` compatibility layout now uses neutral internal
  field names while preserving its 144-byte ABI size.
- Added a blue Settings button and persistent, real ARM64 controls: compatibility fallbacks
  (enabled by default, as before) and sampled missing-API tracing (off by default to reduce
  guest-call overhead). Both settings are passed through the guest service to native execution.
- Split out an ARM32 section that clearly states execution is not packaged; it intentionally
  offers no ineffective switches. ARM32-only bundles remain inspectable but cannot run.
- CI now builds and validates the ARM64 APK on pull requests, checks that the JNI `.so` is inside
  the APK, and publishes release APKs only from `main`.

## 2026-10-03 - PoolAllocator libc++ string fix, POSIX sandbox & EAGL/GLESv2 bridge (batch 4)

Host suite: **473 checks, 0 failures**, clean under `make SAN=1 test` (ASan + UBSan). Targets the
Minecraft PE 0.10.4 `PoolAllocator` / `mod_init` `SIGSEGV` at `faultAddr: 0x10`, sets up POSIX
external storage sandboxing under `/storage/emulated/0/RadekiOSNative/sandbox/`, and prepares the
minimal UIKit / EAGL / OpenGL ES 2.0 rendering bridge.

- **libc++ `basic_string` manipulation & non-null `this` returns** (`runtime/cxx_forward.*`,
  `runtime/stub_dispatch.cpp`): added host implementations for
  `__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKc`
  (`std::string::insert(size_type, const char*)`) and full overload sets for `insert`,
  `push_back`, `append`, `assign`, `reserve`, `resize`, `replace`, `erase`, `operator=`,
  `__grow_by_and_replace`, and `__grow_by`. Mutating methods now return `GuestStdString*` (`this`
  in `x0`) matching the ARM64 Itanium ABI instead of `void` or `0`, resolving the `faultAddr: 0x10`
  null-reference read (`[x0, #0x10]`) in `PoolAllocator` and `mod_init`. Fixed `HostCxxResolver`
  Mach-O `__Z` -> ELF `_Z` symbol stripping for `dlsym`.
- **POSIX external storage sandboxing** (`runtime/compat_libsystem.cpp`, `runtime/runtime.*`):
  dynamic guest-to-sandbox path translation rooted at
  `/storage/emulated/0/RadekiOSNative/sandbox/` with boot-time creation of `Documents/`,
  `Library/Application Support/`, `Library/Caches/`, and `tmp/`. Intercepts C/POSIX file calls
  (`open`, `fopen`, `stat`/`lstat`/`fstat` with 144-byte Darwin ARM64 `struct stat` translation,
  `mkdir`, `access`, `chdir`, `getcwd`, `unlink`, `remove`, `rename`, `rmdir`, `opendir`) and
  Foundation path functions (`NSHomeDirectory`, `NSSearchPathForDirectoriesInDomains`,
  `NSTemporaryDirectory`).
- **Minimal UIKit & EAGL OpenGL ES 2.0 bridge** (`runtime/framework_stubs.*`,
  `android/app/src/main/cpp/CMakeLists.txt`): implemented `EAGLContext` (`initWithAPI:`,
  `setCurrentContext:`, `currentContext`, `presentRenderbuffer:`,
  `renderbufferStorage:fromDrawable:`), `UIWindow` (`makeKeyAndVisible`), and `CADisplayLink`
  (`displayLinkWithTarget:selector:`, `addToRunLoop:forMode:`) bindings via a selector-aware
  `_objc_msgSend` shim, and forwarded OpenGL ES 2.0 entry points (`_gl*`) to Android's native
  `libEGL.so` / `libGLESv2.so` with safe context-free fallbacks.

## 2026-10-03 - libc++ forwarding, framework stubs and the dispatch-stub linker mode (batch 3)

Host suite: **440 checks, 0 failures**, including an ASan/UBSan clean run. Targets the MCPE PE
0.10.4 halt: the guest died in static construction (`__GLOBAL__I_a17`) with SIGTRAP because
unmapped libc++/libSystem exports and missing iOS frameworks left its imports on BRK trap
stubs. ARM64 device execution is still **not** verified here.

- **libc++.1.dylib -> host C++ forwarding** (`runtime/cxx_forward.*`): a symbol mapping table
  registered into the dyld relocation pass maps Apple's mangled C++ exports onto the host C++
  runtime. Explicit wrappers speak Apple's ABI directly for `basic_string` allocation and
  initialization - including the exact symbol from the halt log,
  `__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm` - plus
  constructors/destructors, `assign`/`append`/`reserve`/`resize`/`insert`/`compare`, `npos`
  and `ios_base::Init`, with libc++'s grow-only storage policy. A chained
  `dlsym(RTLD_DEFAULT)` resolver catches remaining `_Z`-mangled imports from Apple C++ dylibs
  (on Android that finds the identical std::__1 symbols in the NDK libc++).
- **Dispatch-stub linker mode** (`runtime/stub_dispatch.*`, `relinker::LinkOptions::stubFactory`,
  `loader::Options::stubFactory`): opt-in; the 446-import class of unbound strong imports is no
  longer forced onto BRK/SIGTRAP trap stubs. Code imports bind to per-symbol 32-byte AArch64
  trampolines in an RW->RX arena that branch to a shared naked dispatcher; it logs the symbol
  (first call always, then sampled) and returns a safe default: 0/NULL/nil, YES/1, the caller's
  x0 (objc retain family / objc_msgSend pass-through) or an empty SEL. Data imports bind to
  labelled dummy objects. Default behaviour is unchanged: honest named BRK traps, and every
  stubbed import stays listed in `LoadResult::unresolved` (with its stub address in the new
  `UnresolvedImport::stub`) and classified `NOOP_STUB` - stubs never fake success.
- **Framework stub layer** (`runtime/framework_stubs.*`): minimal dummy structures/wrappers so
  the startup loop can complete - written shims for `_UIApplicationMain` (returns 0 = completed
  startup), `_NSLog`, the objc memory family, autorelease pools, selector helpers and CF basics;
  `DummyFrameworkClass`/`DummyEAGLContext`/`DummyEAGLSharegroup`/`DummyUIApplication` backing
  ~80 `_OBJC_CLASS_$_`/`_OBJC_METACLASS_$_` imports across UIKit, EAGL, QuartzCore, Foundation,
  AVFoundation and StoreKit; labelled zero blocks for `_NSConcreteStackBlock`, `kCFAllocator*`,
  notification names and similar constants.
- **Loader architecture**: `Registry::resolve()` now documents and implements the full Apple-vs-
  host pipeline (two-level guest images -> flat -> host runtime layer -> unresolved policy);
  `makeHostRuntime()` packages registry + dlsym chain + stub factory for `run` paths. `radeki
  run` and the JNI `run` use it by default (`--traps` restores trap stubs); `load`/`symbols`
  take `--stubs` to preview the mode. New `NOOP_STUB` classification, `RunResult`/
  `MultiRunResult::dispatchStubs`, initializer tracing names each `__mod_init_func` entry.
- **Tests**: +94 checks covering the wrapper ABI against the documented libc++ layout (SSO,
  long mode, grow-only policy, leak-free under ASan), trampoline encodings, dispatcher return
  kinds, both linker modes, honest reporting and determinism across the two link passes, and a
  live-execution test of the trampolines guarded to ARM64 hosts.

### Limits, stated plainly
- No Objective-C dispatch, no GLES/UIKit implementation: stubs keep the loop alive, they do
  not render anything; the compatibility state machine still reports `PARTIALLY_RELINKED` when
  stubs are in use.
- C++ exceptions/unwinding remain unsupported on purpose (`__cxa_throw` etc. stay unregistered;
  with the stub factory they become logging stubs, which is visible but not correct).
- dlsym hits against the NDK libc++ and the whole device path are unverified in this
  environment; host verification is link- and ABI-level only.

## 2026-10-03 - honest compatibility status, C++ shims and offline IPA import (batch 2)

Host suite: **344 checks, 0 failures**, including an ASan/UBSan clean run. Android/NDK builds
and execution on an ARM64 device were **not** verified in this environment.

- Added a 21-row capability matrix, explicit runtime state machine, and per-import classification.
  Parsing or linking never implies a game is launchable; unsupported UIKit/GLES/etc. cannot
  produce `RUNTIME_READY`.
- Added per-image symbol/trap lookup, bounded recent compatibility-call history, ARM64 LR-based
  call-site capture, and a single rich crash-result formatter.
- Added real process-wide C++ exit registration/finalization and guarded initialization,
  operator new/delete shims and distinct C `exit` / immediate `_exit`. Exception handling,
  thread-local destructor semantics and unwinding remain **unsupported** and never pretend to work.
- Added offline DEFLATE/zlib/CRC, PNG (including Apple's CgBI), XML/binary plist, and extracted
  `.app` inspection. Icon selection is restricted to actual bundle contents, with square-icon
  preference and warnings on missing icons; no network or invented art.
- Added `radeki ipa` and `radeki symbols`, plus measured state/capabilities in `analyze`.
- Renamed Android package to `org.radekiosnative.recompiler` and added dark UI, persistent
  library, staged SAF IPA extraction, native metadata/analysis JSON, crash logs and dialog.
  Java/Gradle/NDK compilation and real IPA compatibility still require device/toolchain checks.

### Follow-up hardening
- Bundle icon selection now resolves declared @2x/@3x/~ipad renditions and rejects icons symlinked outside the imported .app.
- Android re-import preserves the prior bundle and icon until replacement inspection and library storage complete; library writes use AtomicFile.
- Fixed Android `.ipa` import failing with `bundle path is not an .app directory` after moving the staged `.app` bundle into `games/<bundleId>/bundle.app`, and taught `ipa::inspectBundle` to accept relocated bundle directories containing `Info.plist` as well as trailing slashes and case-insensitive `.app` / `Info.plist` / executable names.
- Hardened IPA plist/icon/Mach-O import handling (Xcode icon size suffixes, `.png` stem stripping, palette/16-bit PNGs, CgBI trailing padding, threaded bind opcodes `0xD0`, library game removal, and APK version bump to `0.2.0`).

## 2026-10-03 - multi-image loading and Objective-C metadata reading

Test suite: **213 checks, 0 failures**, clean under `make SAN=1 test` (ASan + UBSan).

### Added
- **`loader/dyld.*` - dyld-style image registry.** Register an executable plus any number of
  dylibs; `linkAll()` orders them dependency-first, gives each one a 16K-aligned load base and
  slide, and binds imports across images: two-level lookup by install name, `LC_REEXPORT_DYLIB`
  chains (depth-limited), and flat-namespace lookups. `linkAll()` runs two passes - the first
  fixes each image's layout and size, the second binds against the complete export table - so a
  flat-namespace import can bind to an image registered *after* its importer, the way dyld binds
  only once every image is mapped. Unbound strong imports keep their named BRK trap stubs; failed
  images report why in `LoadResult::errors` and cannot satisfy anyone's import.
- **`objc/objc.*` - read-only Objective-C metadata reading.** Walks `__objc_classlist`,
  `__objc_catlist` and `__objc_protolist` into classes (through `class_ro_t`), metaclasses,
  categories and protocols, including methods in both the classic absolute-pointer (24-byte) and
  the relative (12-byte) encodings, ivars (32/20-byte), properties (16/8-byte) and protocol
  lists (absolute and relative encodings). It is not a runtime: nothing is registered,
  dispatched or called.
- **`runtime::runLoadedImage()`** - maps a whole linked image set into one process reservation
  (so every image keeps its assigned slide), applies per-image W^X protections and runs the
  images in link order, reporting per-image results. Requires an ARM64 host and says so plainly
  when it does not have one.
- **CLI**: `radeki load <exe> [--dylib <path>]...` and `radeki run ...`, plus `analyze --objc`
  for the full metadata dump. Exit code 3 now means "linked, but some imports had no provider";
  unknown options are rejected instead of ignored.
- **libSystem subset** grew: `_strcat`, `_strncat`, `_strstr`, `_strrchr`, `_strtol`, `_strtoul`,
  `_atol`, `_strerror`, common libm entry points, and a pthread subset (`_pthread_create`,
  `_pthread_join`, `_pthread_self`, static non-recursive mutexes) behind written trampolines.

### Changed
- **Validation now walks real instruction ranges.** `relinker::LinkedImage` records the slid
  ranges of sections carrying `S_ATTR_PURE_INSTRUCTIONS`/`S_ATTR_SOME_INSTRUCTIONS`, plus the
  trap stubs and any veneer in use; `validate()` uses those instead of disassembling whole
  executable segments. Mach-O headers, load commands and string constants in `__TEXT` are no
  longer decoded as code, which removes false violations (and stops real violations from hiding
  behind the same effect).
- Test images: `synth::objcImage()` (a full metadata-bearing ARM64 image, 41 rebase fixups) and
  `synth::dylib()` (a minimal dylib exporting `_missing_fn`/`_flat_sym`); `synth::build()` gained
  a flat-namespace (`ordinal -2`) import option.
- `progress.json` and `capabilities.json` updated, including the new gaps.

### Known limits (unchanged or newly documented)
- The runtime has still never executed on ARM64 hardware; the multi-image path is verified at
  link level only.
- No dyld shared cache, no `@rpath`/`@executable_path` expansion, no cross-image initializer
  ordering, no weak-symbol coalescing.
- Objective-C metadata is read but not used for dispatch; there is no `NSObject`, no selector
  table, no class registration.
- ARM64e PAC, ARMv7 AOT, TLS, variadic calls and encrypted binaries remain unsupported.

## 2026-10-02 - initial foundation (as shipped in the first `RadekiOSNative.zip`)

- Mach-O parser: thin/fat, 32/64-bit, load commands, symbols, dyld-info rebase/bind, chained
  fixups, function starts, data-in-code, encryption info; malformed input rejected.
- Analyzer: functions, call/branch/ADRP xrefs, PAC instruction counts, framework and
  ObjC/Swift/Metal/GLES hints, static blockers.
- ARM64 relinker: slide, rebase, bind, trap stubs for unresolved imports, B/BL veneers,
  reference validator.
- Runtime: mmap + W^X + signal-safe call into guest code, libSystem subset, JNI bridge, guest
  process service, on-device self-test button.
- Android app skeleton and a CI workflow that unzips the source, runs the host tests and
  publishes a debug APK as a release.

## Channel notes

- The artifact is `RadekiOSNative.zip` in the repository root; everything under it is the
  project source tree.
- Chronology of a change: edit the source tree → re-zip → commit → CI (`build`) unzips, runs
  `make -C RadekiOSNative SAN=1 test`, builds `assembleDebug` and publishes
  `RadekiOSNative.apk` as release `build-<run number>`.
- A change is only listed here once the host test suite passes; claims that need hardware are
  marked as such in `progress.json` and `capabilities.json`.

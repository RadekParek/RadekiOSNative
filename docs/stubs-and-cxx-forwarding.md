# libc++ forwarding, framework stubs and the dispatch-stub linker mode

This batch targets the failure mode seen when running Minecraft PE 0.10.4 (ARM64 Mach-O):
the guest halts in static construction (`__GLOBAL__I_a17`, guest address `0x1002a188c`,
faulting PC `0x10052c42c`) with SIGTRAP because its imports from `/usr/lib/libc++.1.dylib`,
`/usr/lib/libSystem.B.dylib` and the UIKit/Foundation/CoreGraphics frameworks resolve to
named BRK trap stubs. Three layers now address that, each keeping the project's honesty rule:
**nothing unresolved is ever reported as resolved.**

## 1. Symbol resolution during the relocation pass (`loader/dyld.cpp`)

`Registry::resolve(symbol, dylib)` is the hook the relinker calls for every bind fixup. The
lookup order is:

```
 IPA requests a symbol
        |
        v
 1. two-level namespace: the dylib named by the import's ordinal
    (a registered guest image whose LC_ID_DYLIB matches "/usr/lib/libc++.1.dylib" etc.),
    plus its LC_REEXPORT_DYLIB chains                      -> real guest code wins
        |  miss
        v
 2. flat namespace: every global loaded image in link order (ordinal -2 / 0 / -1)
        |  miss
        v
 3. host runtime layer (Options::fallback -- "Apple library requested, host provides"):
      libSystem.B.dylib subset      -> identical host libc/libm/pthread functions
      libc++abi.dylib               -> written C++ shims (atexit, guards, new/delete)
      libc++.1.dylib                -> std::__1 forwarding table: explicit wrappers with
                                       Apple's exact mangled names + dlsym pass-through onto
                                       the host libc++ (libc++_shared on Android, identical
                                       std::__1 mangling and ABI)
      UIKit/Foundation/EAGL/etc.    -> hand-written shims (NSLog, UIApplicationMain, the objc
                                       memory family) + dummy _OBJC_CLASS_$_ objects
        |  miss
        v
 4. unresolved policy:
      default        -> named BRK trap stub (SIGTRAP on call, reports the symbol)
      stubFactory    -> host-executable logging no-op stub (guest keeps running)
```

Steps 1-2 only answer from real guest images; steps 3-4 are the only places host addresses
enter guest bindings. Weak imports bind to null in every mode, matching dyld.

## 2. libc++.1.dylib -> libc++_shared forwarding (`runtime/cxx_forward.*`)

Apple's libc++ and the NDK's libc++ are the same upstream project: same `std::__1` inline
namespace, same Itanium ABI, same `basic_string` storage layout (24 bytes; short mode stores
`size << 1` in byte 0 with up to 22 inline chars, long mode stores `(capacity << 1) | 1`,
size and the data pointer). So the mapping is mostly "identical mangling, forward to a host
implementation", done two ways:

* **Explicit table** (`cxxForwardTable()`, registered by `addCxxForwarding()` into the compat
  registry, classification `ANDROID_BACKEND` / framework `libc++`). The wrappers are written
  against Apple's ABI directly -- they never reinterpret the host's `std::string`, because the
  host may be libstdc++ (different layout). Covered: the symbol from the halt log,
  `__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm`
  (`basic_string::__init(const char*, size_type)`), plus `__init(size, char)`, default/copy/
  C-string constructors (C1/C2 incl. the allocator-taking form), destructors, `operator=`,
  `assign` (ptr/len, fill, from-string), `append`, `push_back`, `reserve`, `resize`,
  `insert`, `compare(const char*)`, the `npos` data symbol, and `ios_base::Init` C1/C2/D1/D2
  (the classic `__GLOBAL__I_a` dependency). Allocation policy mirrors libc++: storage only
  grows, so guest-held `data()` pointers stay valid exactly as long as real libc++ keeps them.
* **dlsym pass-through** (`HostCxxResolver`, chained behind the registry via
  `CompatRegistry::chainNext()`): for `_Z`-mangled symbols requested from Apple C++ dylibs
  that the table does not name, `dlsym(RTLD_DEFAULT, ...)` probes the host process. On an
  Android device the NDK libc++ (c++_static or c++_shared) is linked into this very process
  and exports the identical symbols, so the probe finds real, ABI-compatible
  implementations. Misses stay honest.

Deliberately unregistered: `__cxa_throw` and the exception/unwinding family -- a silent no-op
throw would corrupt the guest far from the call site. With the stub factory enabled those
become logging stubs instead, which is still visible.

## 3. Framework stubs and the dispatch-stub linker mode

`runtime/framework_stubs.*` registers the hand-written end:

* written shims (`COMPATIBILITY_SHIM`, method `shim`): `_UIApplicationMain` (logs that there
  is no UIKit event loop/screen and returns 0 only to let the guest tear down), `_NSLog`, the
  objc memory family (`objc_retain` & co. pass
  the object through instead of nil-ing it; `objc_release`/`objc_storeStrong` are no-ops;
  `objc_autoreleasePoolPush/Pop` hand out a stable token), `sel_registerName`/`sel_getUid`,
  `objc_getClass` -> nil, `objc_exception_throw` -> aborts the run with a message, and a set
  of CoreFoundation/UIKit/QuartzCore basics;
* dummy C++ structures backing data imports: `DummyFrameworkClass` (objc-class-shaped, all
  real fields zero, class name kept for diagnostics) for ~80 UIKit/Foundation/QuartzCore/EAGL/
  AVFoundation/StoreKit classes as `_OBJC_CLASS_$_` / `_OBJC_METACLASS_$_`, plus specialized
  `DummyEAGLContext`, `DummyEAGLSharegroup` and `DummyUIApplication` wrappers, and labelled
  zero blocks for constants (`_NSConcreteStackBlock`, `kCFAllocator*`, notification names,
  `_NSFoundationVersionNumber`, ...). Classification `NOOP_STUB`, methods `dummy_class` /
  `dummy_data`.

Everything the table does not name goes through the **generic dispatcher**
(`runtime/stub_dispatch.*`), enabled by handing a `StubFactory` to the link:

* code imports get a 32-byte AArch64 trampoline emitted into an RW->RX arena:
  `LDR x9, =record ; LDR x16, =dispatcher ; BR x16`. The naked dispatcher preserves the
  guest's x29/x30 and callee-saved registers, calls the logging C handler, then returns a
  safe default per symbol: `0/NULL/nil` (default), `YES/1`, the caller's `x0` (objc retain
  family, `objc_msgSend`, so object graphs survive), or an empty-SEL pointer. First call of
  every symbol is logged to the guest output ring and logcat; later calls are sampled.
* data imports get the dummy objects described above.
* Stub addresses are deduplicated per symbol, so the loader's two link passes stay consistent.

Binding mode is a `LinkOptions::stubFactory` decision in `relinker::link()`: **default stays
the honest BRK trap** (every existing guarantee and test), while `radeki run`, the JNI `run`
path and `--stubs` enable dispatch stubs. In either mode the import is reported:
`BoundImport.dispatchStub`, `LoadResult::unresolved` entries carry the stub address in
`UnresolvedImport::stub`, `linkAll()` adds a warning naming the count, `RunResult` /
`MultiRunResult` expose `dispatchStubs`, and the state machine still sees
`unresolvedStrong > 0` -> `PARTIALLY_RELINKED`. Stubs let the startup loop continue; they do
not upgrade the compatibility state.

## Follow-up: MCPE `ChunkPos` hash-table SIGSEGV

A later device log showed `__hash_table<ChunkPos>::__insert_unique` calling the unresolved
libc++ import `__ZNSt3__112__next_primeEm`. It had been bound to a generic no-op stub and
returned zero, which cannot be used as a hash-table bucket count. The explicit C++ forwarding
table now provides this symbol with the libc++ contract (`0` remains the empty-table sentinel;
otherwise it returns the smallest prime at least the requested bucket count), using deterministic
64-bit Miller-Rabin so even unusually large requests do not trigger unbounded trial division.
This addresses that specific crash path; it is not evidence
that a complete Minecraft launch now works.

Recent-call history now uses a fixed 32-entry, fixed-size ring and has a `const char*` overload,
so frequent compatibility wrappers do not allocate temporary strings just to record diagnostic
names. Stub call tracing remains independently configurable, and `runImage()` no longer builds
a redundant second compatibility registry in fallback mode.

## UIKit status and launch limits

The analyzer continues to report **UIKit unsupported**. That is intentional and accurate: the
runtime has a few startup shims and dummy class objects, not a UIKit object model, view tree,
application event loop, or Android surface presentation path. In particular, the placeholder
`UIApplicationMain` returns immediately and does not render a boot screen. Similar limits remain
for Foundation/CoreFoundation and full EAGL integration. These gaps require real subsystem work;
changing the capability label or returning success from no-op functions would only hide failures.

## Verified / not verified

Host-verified by the test suite (486 checks, ASan/UBSan clean): string wrapper behavior against
the documented libc++ layout (SSO, long mode, growth-only policy), prime results across small
bucket counts and a 64-bit boundary case, fixed call-history behavior, trampoline encodings and
dispatcher return kinds, linker/loader reporting in both modes, and honest behavior on non-ARM64
hosts. Not verified (needs Android toolchain/device): NDK libc++ dlsym hits, the new symbol on an
ARM64 device, or Minecraft PE reaching `main()` / a visible boot screen.

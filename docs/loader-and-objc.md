# Loader and Objective-C metadata: what they do, and what they deliberately do not

Two modules were added for the multi-image milestone. Both are written so that the gap between
"works" and "not done" is visible in the API, not just in the README.

## `loader/dyld.*` - an image registry, not a dyld

`Registry::add(Spec)` records an image (executable or dylib). `Registry::linkAll(Options)` then:

1. orders the images dependency-first, using each image's `LC_LOAD_DYLIB` list;
2. gives every image a 16K-aligned load base and computes its slide;
3. publishes the image's defined external symbols as exports;
4. calls `relinker::link()` with the registry itself as the resolver.

Because `relinker::link()` is unchanged, an import that cannot be satisfied still becomes a named
BRK trap stub. Adding an image can therefore only turn a trap into a real address; it can never
turn an unresolved symbol into a silent fake success.

`linkAll()` runs twice. Pass one lays the images out (a slide is needed to publish exports at
their final addresses, and a size is needed to place the next image). Pass two republishes the
complete export table and relinks, so a flat-namespace import whose provider is registered after
its importer still binds - which is what dyld does, since it binds only after every image is
mapped. Binding never changes an image's size, so the pass-one layout stays valid.

Lookup order for `resolve(symbol, dylib)`: the named image (with `LC_REEXPORT_DYLIB` chains,
depth-limited to 8 and cycle-safe), then - for flat lookups (ordinal 0 / `-2`) or when the named
image has nothing - every global image in link order, then the fallback resolver: the host
runtime layer, where Apple system-library imports land when no loaded image provides them
(libSystem subset, libc++abi shims, the libc++ -> host forwarding table, framework shims and
dummy class objects; see `stubs-and-cxx-forwarding.md`). What still misses hits the relinker's
unresolved policy: named BRK traps by default, or - with `Options::stubFactory` set - logging
no-op dispatch stubs. Both keep listing the import as unresolved.

Not implemented, on purpose and by omission: no dyld shared cache, no `@rpath`/`@loader_path`
expansion against a filesystem, no initializer ordering *between* images, no weak-symbol
coalescing, no lazy binding, no per-image ASLR beyond the bases the caller asks for.

`runtime::runLoadedImage()` re-links through the registry and runs the whole set inside one
`mmap` reservation, so every image keeps the slide the loader computed. Per-image protections are
applied while the mapping is writable and the instruction cache is flushed before the first jump,
which keeps the W^X discipline of the single-image path. An image that calls `_exit`/`_abort`
ends the run: images after it are reported as `skipped`, never started behind the guest's back.

## `objc/objc.*` - reading metadata, not implementing the runtime

`objc::parse(img)` walks `__objc_classlist`, `__objc_catlist` and `__objc_protolist` and returns
plain structs: classes (through `class_data_bits_t` -> `class_ro_t`, with the metaclass read one
level deep for class methods), categories, protocols, method lists, ivars, properties and the
strings in `__objc_methname`.

Encodings accepted: method lists in the classic 24-byte absolute-pointer form and the relative
12-byte form; ivar lists in 32- and 20-byte form; property lists in 16- and 8-byte form; protocol
lists in both the absolute and the relative layout. A method list with any other entry size is
refused with a warning - the parser never guesses an encoding.

The contract that matters: **`parse()` does not throw for malformed metadata.** Every unreadable
pointer, out-of-image address, tagged/PAC pointer in `class_data_bits_t`, implausible count or
unknown entry size becomes a string in `Metadata::warnings` and flips `Metadata::complete` to
false. Tags and PAC bits are detected (a class-data pointer with bits outside the fast-data mask
is reported, not followed). The test suite enforces this by mutating the metadata image 3000
times and failing if a `FormatError` ever escapes.

It is not a runtime. There is no class registration, no method cache, no `objc_msgSend`, no
`NSObject`, no selector table, no `+load`/`+initialize` invocation, no `__objc_classname`/
`__objc_superrefs` cross-validation yet. Reading metadata is the prerequisite for those, not a
substitute.

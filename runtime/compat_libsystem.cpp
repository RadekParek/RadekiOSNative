#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#include "runtime/runtime.h"
#include "runtime/compat_cxx.h"
#include "runtime/cxx_forward.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace radeki::runtime {
namespace {
std::mutex g_outMutex;
std::string g_out;
uint64_t g_stackGuard = 0x2F4A1B9C7D3E5A60ull;

std::mutex g_sandboxMutex;
std::string g_sandboxBase = kDefaultSandboxBase;
std::string g_guestCwd = "/storage/emulated/0/RadekiOSNative/sandbox/Documents";
bool g_useFallbackMirror = false;      // logical base is kDefaultSandboxBase; I/O maps to mirror
std::string g_fallbackMirrorPath;      // the actual host writable location when falling back

std::string normalizeSlash(std::string p, bool trailingSlash) {
  if (p.empty()) p = kDefaultSandboxBase;
  // Collapse duplicate slashes.
  std::string out;
  out.reserve(p.size() + 1);
  for (char c : p) {
    if (c == '/' && !out.empty() && out.back() == '/') continue;
    out.push_back(c);
  }
  if (trailingSlash) {
    if (out.back() != '/') out.push_back('/');
  } else {
    while (out.size() > 1 && out.back() == '/') out.pop_back();
  }
  return out;
}

std::string scopedFallbackBase() {
  return normalizeSlash(std::string(kScopedFallbackSandboxBase), true);
}

std::string hostTempMirrorBase() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path tmp = fs::temp_directory_path(ec);
  if (ec || tmp.empty()) tmp = "/tmp";
  return normalizeSlash((tmp / "RadekiOSNative" / "sandbox").string(), true);
}

// Interned string/array pool so pointers returned by NSHomeDirectory /
// NSSearchPathForDirectoriesInDomains stay valid for the process lifetime and work both as
// direct C-strings and as receivers in objc_msgSend(obj, "objectAtIndex:" / "UTF8String").
struct SandboxPathObject {
  char cstr[512] = {};  // offset 0 is the NUL-terminated UTF-8 path itself
};

std::mutex g_internMutex;
std::deque<SandboxPathObject> g_internedPaths;

const char* internSandboxPath(const std::string& s) {
  std::lock_guard<std::mutex> lock(g_internMutex);
  for (const auto& e : g_internedPaths) {
    if ( s == e.cstr ) return e.cstr;
  }
  g_internedPaths.emplace_back();
  auto& back = g_internedPaths.back();
  std::snprintf(back.cstr, sizeof back.cstr, "%s", s.c_str());
  return back.cstr;
}

}  // namespace

std::string sandboxBasePath() {
  std::lock_guard<std::mutex> lock(g_sandboxMutex);
  return normalizeSlash(g_sandboxBase, true);
}

void setSandboxBasePath(const std::string& path) {
  {
    std::lock_guard<std::mutex> lock(g_sandboxMutex);
    g_sandboxBase = path.empty() ? std::string(kDefaultSandboxBase) : normalizeSlash(path, true);
    g_guestCwd = normalizeSlash(g_sandboxBase + "Documents", false);
    g_useFallbackMirror = false;
  }
  ensureSandboxDirectories();
}

std::vector<std::string> standardSandboxDirectories() {
  std::string base = sandboxBasePath();
  return {
      base + "Documents/",
      base + "Documents/games/com.mojang/",
      base + "Library/Application Support/",
      base + "Library/Caches/",
      base + "tmp/",
  };
}

// Returns true iff all standard directories exist on the given base (after create_directories).
bool tryCreateSandboxAt(const std::string& base) {
  namespace fs = std::filesystem;
  bool allOk = true;
  const char* rels[] = {"Documents/",
                        "Documents/games/com.mojang/",
                        "Library/Application Support/",
                        "Library/Caches/",
                        "tmp/"};
  for (const char* rel : rels) {
    std::error_code ec;
    std::string p = base + rel;
    fs::create_directories(p, ec);
    if (ec && !fs::is_directory(p, ec)) allOk = false;
  }
  return allOk;
}

bool ensureSandboxDirectories() {
  namespace fs = std::filesystem;
  const std::string defaultBase = normalizeSlash(kDefaultSandboxBase, true);
  // If the caller has set an explicit sandbox base (non-default), ensure it exists directly.
  {
    std::lock_guard<std::mutex> lock(g_sandboxMutex);
    if (g_sandboxBase != defaultBase) {
      g_useFallbackMirror = false;
      g_fallbackMirrorPath.clear();
      return tryCreateSandboxAt(g_sandboxBase);
    }
  }
  // 1) Primary path: /storage/emulated/0/RadekiOSNative/sandbox/
  if (tryCreateSandboxAt(defaultBase)) {
    std::lock_guard<std::mutex> lock(g_sandboxMutex);
    g_useFallbackMirror = false;
    g_fallbackMirrorPath.clear();
    g_sandboxBase = defaultBase;
    g_guestCwd = normalizeSlash(g_sandboxBase + "Documents", false);
    return true;
  }
  // 2) Scoped-storage fallback: /storage/emulated/0/Android/data/org.radekiosnative.recompiler/files/sandbox/
  std::string scoped = scopedFallbackBase();
  if (tryCreateSandboxAt(scoped)) {
    std::lock_guard<std::mutex> lock(g_sandboxMutex);
    g_useFallbackMirror = true;
    g_fallbackMirrorPath = scoped;
    g_sandboxBase = defaultBase;  // logical base unchanged
    g_guestCwd = normalizeSlash(g_sandboxBase + "Documents", false);
    return true;
  }
  // 3) Host-temp mirror (for non-Android build hosts / restricted environments).
  std::string mirror = hostTempMirrorBase();
  bool mirrorOk = tryCreateSandboxAt(mirror);
  {
    std::lock_guard<std::mutex> lock(g_sandboxMutex);
    g_useFallbackMirror = mirrorOk;
    g_fallbackMirrorPath = mirrorOk ? mirror : "";
    g_sandboxBase = defaultBase;
    g_guestCwd = normalizeSlash(g_sandboxBase + "Documents", false);
  }
  return mirrorOk;
}

std::string translateGuestPath(const std::string& guestPath) {
  std::string base = sandboxBasePath();
  std::string baseNoSlash = normalizeSlash(base, false);
  if (guestPath.empty() || guestPath == "." || guestPath == "./") {
    std::lock_guard<std::mutex> lock(g_sandboxMutex);
    return g_guestCwd;
  }
  if (guestPath == "~") return baseNoSlash;
  if (guestPath.rfind("~/", 0) == 0) {
    return normalizeSlash(base + guestPath.substr(2), false);
  }
  // Already inside the configured sandbox base or default /storage/emulated/0/RadekiOSNative/sandbox:
  if (guestPath == baseNoSlash || guestPath.rfind(base, 0) == 0) {
    return normalizeSlash(guestPath, false);
  }
  const std::string defBase = kDefaultSandboxBase;
  const std::string defNoSlash = normalizeSlash(defBase, false);
  if (guestPath == defNoSlash) return baseNoSlash;
  if (guestPath.rfind(defBase, 0) == 0) {
    return normalizeSlash(base + guestPath.substr(defBase.size()), false);
  }

  std::string p = guestPath;
  if (p.rfind("/private/", 0) == 0) {
    p = p.substr(std::strlen("/private"));
  }

  // Known iOS container prefixes where an application UUID directory sits before Documents/Library/tmp:
  auto stripAppUuidPrefix = [&](const char* prefix) -> std::optional<std::string> {
    size_t len = std::strlen(prefix);
    if (p.rfind(prefix, 0) != 0) return std::nullopt;
    std::string rest = p.substr(len);
    size_t slash = rest.find('/');
    if (slash == std::string::npos) return std::string("");
    return rest.substr(slash + 1);
  };

  for (const char* appPrefix : {
           "/var/mobile/Applications/",
           "/var/mobile/Containers/Data/Application/",
           "/var/mobile/Containers/Bundle/Application/",
           "/var/containers/Data/Application/",
           "/var/containers/Bundle/Application/",
       }) {
    if (auto rel = stripAppUuidPrefix(appPrefix)) {
      if (rel->empty()) return baseNoSlash;
      return normalizeSlash(base + *rel, false);
    }
  }

  if (p == "/var/mobile" || p == "/var/mobile/") return baseNoSlash;
  if (p.rfind("/var/mobile/", 0) == 0) {
    return normalizeSlash(base + p.substr(std::strlen("/var/mobile/")), false);
  }
  if (p == "/tmp" || p == "/var/tmp") {
    return normalizeSlash(base + "tmp", false);
  }
  if (p.rfind("/tmp/", 0) == 0) {
    return normalizeSlash(base + "tmp/" + p.substr(5), false);
  }
  if (p.rfind("/var/tmp/", 0) == 0) {
    return normalizeSlash(base + "tmp/" + p.substr(9), false);
  }

  // Direct iOS container subdirectories (/Documents, /Library, Documents, Library, tmp)
  for (const char* sub : {"/Documents", "/Library", "/tmp"}) {
    size_t len = std::strlen(sub);
    if (p == sub || (p.rfind(sub, 0) == 0 && p[len] == '/')) {
      return normalizeSlash(base + p.substr(1), false);
    }
  }
  for (const char* sub : {"Documents", "Library", "tmp"}) {
    size_t len = std::strlen(sub);
    if (p == sub || (p.rfind(sub, 0) == 0 && p[len] == '/')) {
      return normalizeSlash(base + p, false);
    }
  }

  // Relative paths resolve against the guest's current working directory inside the sandbox.
  if (p[0] != '/') {
    if (p.rfind("./", 0) == 0) p = p.substr(2);
    std::string cwd;
    {
      std::lock_guard<std::mutex> lock(g_sandboxMutex);
      cwd = g_guestCwd;
    }
    return normalizeSlash(cwd + "/" + p, false);
  }

  // Any other iOS absolute path is mapped inside the sandbox container.
  return normalizeSlash(base + p.substr(1), false);
}

std::string resolveSandboxHostPath(const std::string& guestOrSandboxPath) {
  ensureSandboxDirectories();
  std::string logical = translateGuestPath(guestOrSandboxPath);
  std::string base = sandboxBasePath();
  bool useMirror = false;
  std::string mirrorPath;
  {
    std::lock_guard<std::mutex> lock(g_sandboxMutex);
    useMirror = g_useFallbackMirror;
    mirrorPath = g_fallbackMirrorPath;
  }
  if (!useMirror || mirrorPath.empty()) return logical;
  std::string baseNoSlash = normalizeSlash(base, false);
  std::string mirrorNoSlash = normalizeSlash(mirrorPath, false);
  if (logical == baseNoSlash) return mirrorNoSlash;
  if (logical.rfind(base, 0) == 0) {
    return normalizeSlash(mirrorPath + logical.substr(base.size()), false);
  }
  return logical;
}

const char* guestNSHomeDirectory() {
  noteCompatCall("NSHomeDirectory");
  ensureSandboxDirectories();
  std::string home = normalizeSlash(sandboxBasePath(), false);
  return internSandboxPath(home);
}

const char* guestNSTemporaryDirectory() {
  noteCompatCall("NSTemporaryDirectory");
  ensureSandboxDirectories();
  return internSandboxPath(sandboxBasePath() + "tmp/");
}

const void* guestNSSearchPathForDirectoriesInDomains(uint64_t directory, uint64_t /*domainMask*/, int /*expandTilde*/) {
  noteCompatCall("NSSearchPathForDirectoriesInDomains");
  ensureSandboxDirectories();
  std::string base = sandboxBasePath();
  std::string target;
  switch (directory) {
    case 9:   // NSDocumentDirectory
      target = normalizeSlash(base + "Documents", false);
      break;
    case 5:   // NSLibraryDirectory
      target = normalizeSlash(base + "Library", false);
      break;
    case 13:  // NSCachesDirectory
      target = normalizeSlash(base + "Library/Caches", false);
      break;
    case 14:  // NSApplicationSupportDirectory
      target = normalizeSlash(base + "Library/Application Support", false);
      break;
    default:
      target = normalizeSlash(base + "Documents", false);
      break;
  }
  return internSandboxPath(target);
}

namespace {

// Guest output has exactly one reader (runtime::run), but imports can be called from any
// thread, so every append is serialised by g_outMutex.

int c_puts(const char* s) {
  noteCompatCall("puts");
  appendGuestOutput(std::string(s ? s : "(null)") + "\n");
  return 1;
}
int c_putchar(int c) { appendGuestOutput(std::string(1, char(c))); return c; }
void c_exit(int code) { finalizeCxx(nullptr); abortGuest(code, nullptr); }
void c_immediate_exit(int code) { abortGuest(code, nullptr); }
void c_abort() { abortGuest(134, "abort() called"); }
void c_stack_chk_fail() { abortGuest(134, "stack check failed"); }
int c_abs(int x) { return x < 0 ? -x : x; }
char* c_strchr(const char* s, int c) { return const_cast<char*>(std::strchr(s, c)); }
void* c_memcpy_chk(void* d, const void* s, size_t n, size_t dl) {
  if (n > dl) abortGuest(134, "__memcpy_chk overflow");
  return std::memcpy(d, s, n);
}
void* c_memset_chk(void* d, int c, size_t n, size_t dl) {
  if (n > dl) abortGuest(134, "__memset_chk overflow");
  return std::memset(d, c, n);
}

// --- POSIX External Storage Sandboxing Interceptors ----------------------------------------

// Darwin ARM64 struct stat (144 bytes).
struct DarwinStat64 {
  // Neutral field names avoid Bionic's st_atime_nsec/st_mtime_nsec/st_ctime_nsec macros.
  // The names are internal; offsets and widths preserve Darwin ARM64's 144-byte ABI layout.
  int32_t device;
  uint16_t mode;
  uint16_t linkCount;
  uint64_t inode;
  uint32_t uid;
  uint32_t gid;
  int32_t specialDevice;
  int32_t pad0;
  int64_t accessTimeSeconds;
  int64_t accessTimeNanoseconds;
  int64_t modificationTimeSeconds;
  int64_t modificationTimeNanoseconds;
  int64_t changeTimeSeconds;
  int64_t changeTimeNanoseconds;
  int64_t birthTimeSeconds;
  int64_t birthTimeNanoseconds;
  int64_t size;
  int64_t blocks;
  int32_t blockSize;
  uint32_t flags;
  uint32_t generation;
  int32_t spare;
  int64_t reserved[2];
};
static_assert(sizeof(DarwinStat64) == 144, "Darwin arm64 struct stat is 144 bytes");

void fillDarwinStat(void* dst, const struct stat& hostSt) {
  if (!dst) return;
  DarwinStat64 ds{};
  ds.device = static_cast<int32_t>(hostSt.st_dev);
  ds.mode = static_cast<uint16_t>(hostSt.st_mode);
  ds.linkCount = static_cast<uint16_t>(hostSt.st_nlink);
  ds.inode = static_cast<uint64_t>(hostSt.st_ino);
  ds.uid = static_cast<uint32_t>(hostSt.st_uid);
  ds.gid = static_cast<uint32_t>(hostSt.st_gid);
  ds.specialDevice = static_cast<int32_t>(hostSt.st_rdev);
  ds.accessTimeSeconds = static_cast<int64_t>(hostSt.st_atim.tv_sec);
  ds.accessTimeNanoseconds = static_cast<int64_t>(hostSt.st_atim.tv_nsec);
  ds.modificationTimeSeconds = static_cast<int64_t>(hostSt.st_mtim.tv_sec);
  ds.modificationTimeNanoseconds = static_cast<int64_t>(hostSt.st_mtim.tv_nsec);
  ds.changeTimeSeconds = static_cast<int64_t>(hostSt.st_ctim.tv_sec);
  ds.changeTimeNanoseconds = static_cast<int64_t>(hostSt.st_ctim.tv_nsec);
  ds.birthTimeSeconds = ds.changeTimeSeconds;
  ds.birthTimeNanoseconds = ds.changeTimeNanoseconds;
  ds.size = static_cast<int64_t>(hostSt.st_size);
  ds.blocks = static_cast<int64_t>(hostSt.st_blocks);
  ds.blockSize = static_cast<int32_t>(hostSt.st_blksize);
  std::memcpy(dst, &ds, sizeof ds);
}

// Translate Darwin open() flags to Linux/Android open() flags, while also preserving host
// flags when called directly from host tests.
int translateOpenFlags(int flags) {
  int out = flags & O_ACCMODE;
  // Darwin: O_NONBLOCK=0x0004, O_APPEND=0x0008, O_CREAT=0x0200, O_TRUNC=0x0400, O_EXCL=0x0800
  if (flags & 0x0004) out |= O_NONBLOCK;
  if (flags & 0x0008) out |= O_APPEND;
  if (flags & 0x0200) out |= O_CREAT;
  if (flags & 0x0400) out |= O_TRUNC;
  if (flags & 0x0800) out |= O_EXCL;
  // Also accept Linux O_CREAT (0x40) / O_EXCL (0x80) if no conflicting Darwin bits were meant.
  if (flags & O_CREAT) out |= O_CREAT;
  if (flags & O_EXCL) out |= O_EXCL;
  if ((flags & O_ACCMODE) != O_RDONLY && (flags & (0x0200 | O_CREAT))) {
    // If caller asked for write+create with 0x0400 (Darwin O_TRUNC) or 0x0200 (Linux O_TRUNC),
    // honor truncation.
    if (flags & 0x0400) out |= O_TRUNC;
  }
  return out;
}

void ensureParentDirectory(const std::string& hostPath) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path p(hostPath);
  if (p.has_parent_path()) {
    fs::create_directories(p.parent_path(), ec);
  }
}

int c_open(const char* path, int flags, mode_t mode) {
  noteCompatCall("open");
  if (!path) { errno = EFAULT; return -1; }
  std::string hostPath = resolveSandboxHostPath(path);
  int hostFlags = translateOpenFlags(flags);
  if (hostFlags & O_CREAT) {
    ensureParentDirectory(hostPath);
    if (mode == 0) mode = 0666;
  }
  return ::open(hostPath.c_str(), hostFlags, mode);
}

FILE* c_fopen(const char* path, const char* mode) {
  noteCompatCall("fopen");
  if (!path || !mode) { errno = EINVAL; return nullptr; }
  std::string hostPath = resolveSandboxHostPath(path);
  if (std::strchr(mode, 'w') || std::strchr(mode, 'a') || std::strchr(mode, '+')) {
    ensureParentDirectory(hostPath);
  }
  return std::fopen(hostPath.c_str(), mode);
}

int c_stat(const char* path, void* buf) {
  noteCompatCall("stat");
  if (!path) { errno = EFAULT; return -1; }
  std::string hostPath = resolveSandboxHostPath(path);
  struct stat st{};
  int rc = ::stat(hostPath.c_str(), &st);
  if (rc == 0 && buf) fillDarwinStat(buf, st);
  return rc;
}

int c_lstat(const char* path, void* buf) {
  noteCompatCall("lstat");
  if (!path) { errno = EFAULT; return -1; }
  std::string hostPath = resolveSandboxHostPath(path);
  struct stat st{};
  int rc = ::lstat(hostPath.c_str(), &st);
  if (rc == 0 && buf) fillDarwinStat(buf, st);
  return rc;
}

int c_fstat(int fd, void* buf) {
  noteCompatCall("fstat");
  struct stat st{};
  int rc = ::fstat(fd, &st);
  if (rc == 0 && buf) fillDarwinStat(buf, st);
  return rc;
}

int c_mkdir(const char* path, mode_t mode) {
  noteCompatCall("mkdir");
  if (!path) { errno = EFAULT; return -1; }
  std::string hostPath = resolveSandboxHostPath(path);
  ensureParentDirectory(hostPath);
  int rc = ::mkdir(hostPath.c_str(), mode ? mode : 0777);
  if (rc != 0 && errno == EEXIST) {
    struct stat st{};
    if (::stat(hostPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      errno = EEXIST;
    }
  }
  return rc;
}

int c_access(const char* path, int mode) {
  noteCompatCall("access");
  if (!path) { errno = EFAULT; return -1; }
  std::string hostPath = resolveSandboxHostPath(path);
  return ::access(hostPath.c_str(), mode);
}

int c_chdir(const char* path) {
  noteCompatCall("chdir");
  if (!path) { errno = EFAULT; return -1; }
  std::string logical = translateGuestPath(path);
  std::string hostPath = resolveSandboxHostPath(logical);
  struct stat st{};
  if (::stat(hostPath.c_str(), &st) != 0) return -1;
  if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
  std::lock_guard<std::mutex> lock(g_sandboxMutex);
  g_guestCwd = logical;
  return 0;
}

char* c_getcwd(char* buf, size_t size) {
  noteCompatCall("getcwd");
  std::string cwd;
  {
    std::lock_guard<std::mutex> lock(g_sandboxMutex);
    cwd = g_guestCwd;
  }
  if (!buf) {
    return ::strdup(cwd.c_str());
  }
  if (size <= cwd.size()) {
    errno = ERANGE;
    return nullptr;
  }
  std::memcpy(buf, cwd.c_str(), cwd.size() + 1);
  return buf;
}

int c_unlink(const char* path) {
  noteCompatCall("unlink");
  if (!path) { errno = EFAULT; return -1; }
  return ::unlink(resolveSandboxHostPath(path).c_str());
}

int c_remove(const char* path) {
  noteCompatCall("remove");
  if (!path) { errno = EFAULT; return -1; }
  return std::remove(resolveSandboxHostPath(path).c_str());
}

int c_rmdir(const char* path) {
  noteCompatCall("rmdir");
  if (!path) { errno = EFAULT; return -1; }
  return ::rmdir(resolveSandboxHostPath(path).c_str());
}

int c_rename(const char* oldPath, const char* newPath) {
  noteCompatCall("rename");
  if (!oldPath || !newPath) { errno = EFAULT; return -1; }
  std::string hostOld = resolveSandboxHostPath(oldPath);
  std::string hostNew = resolveSandboxHostPath(newPath);
  ensureParentDirectory(hostNew);
  return std::rename(hostOld.c_str(), hostNew.c_str());
}

DIR* c_opendir(const char* path) {
  noteCompatCall("opendir");
  if (!path) { errno = EFAULT; return nullptr; }
  return ::opendir(resolveSandboxHostPath(path).c_str());
}

// --- pthread subset -------------------------------------------------------------------
// A trampoline is needed instead of a direct bind: the guest passes the Apple-sized
// pthread_attr_t and a handler whose signature is not ours (4 arguments there, 3 here).
struct ThreadJob {
  void* (*fn)(void*) = nullptr;
  void* arg = nullptr;
  void* ret = nullptr;
};

void* threadEntry(void* p) {
  auto* job = static_cast<ThreadJob*>(p);
  job->ret = job->fn ? job->fn(job->arg) : nullptr;
  return job->ret;
}

int c_pthread_create(uint64_t* threadOut, const void* attr, uint64_t start, void* arg) {
  noteCompatCall("pthread_create");
  (void)attr;  // Apple's pthread_attr_t layout is not our host's; only the default is honoured
  auto* job = new ThreadJob{reinterpret_cast<void* (*)(void*)>(start), arg, nullptr};
  pthread_t th;
  int rc = pthread_create(&th, nullptr, threadEntry, job);
  if (rc != 0) {
    delete job;
    return rc;
  }
  if (threadOut) {
    // pthread_t is an opaque pointer on Apple and an unsigned long on Linux; both fit in 8 bytes.
    uint64_t v = 0;
    static_assert(sizeof(th) <= sizeof v, "pthread_t must fit in a 64-bit guest slot");
    std::memcpy(&v, &th, sizeof th);
    *threadOut = v;
  }
  return 0;
}

int c_pthread_join(uint64_t thread, void** retval) {
  pthread_t th;
  std::memcpy(&th, &thread, sizeof th);
  int rc = pthread_join(th, retval);
  return rc;
}

uint64_t c_pthread_self() {
  uint64_t v = 0;
  pthread_t th = pthread_self();
  std::memcpy(&v, &th, sizeof th);
  return v;
}

// --- TLS key management: forward directly to host pthread_key_* ---------------------------
// The guest sees pthread_key_t as an opaque 32-bit value that fits in 64 bits. Both Darwin
// and Bionic/Linux treat it as an integer key index, so we can just cast.
int c_pthread_key_create(uint64_t* keyOut, void (*destructor)(void*)) {
  noteCompatCall("pthread_key_create");
  if (!keyOut) return EINVAL;
  pthread_key_t k = 0;
  int rc = pthread_key_create(&k, destructor);
  *keyOut = static_cast<uint64_t>(k);
  return rc;
}

int c_pthread_key_delete(uint64_t key) {
  noteCompatCall("pthread_key_delete");
  return pthread_key_delete(static_cast<pthread_key_t>(key));
}

void* c_pthread_getspecific(uint64_t key) {
  return pthread_getspecific(static_cast<pthread_key_t>(key));
}

int c_pthread_setspecific(uint64_t key, const void* value) {
  return pthread_setspecific(static_cast<pthread_key_t>(key), value);
}

// --- Host-backed pthread_mutex table (used by guest pthread_mutex_* and libc++ std::mutex) -
// We allocate a real host pthread_mutex_t per guest mutex slot, so destruction and recursive
// semantics (and the host kernel's atomicity guarantees) behave correctly. The 64-bit slot
// the guest sees is an ID into this table; zero is the "not yet allocated" sentinel.
struct HostMutex {
  pthread_mutex_t mtx;
  bool initialized;
  HostMutex() : mtx{}, initialized(false) {}
};

struct MutexTable {
  std::mutex table;
  std::map<uint64_t, std::unique_ptr<HostMutex>> slots;
  uint64_t nextId = 1;  // 0 reserved: "uninitialized"
};
MutexTable& mutexTable() {
  static MutexTable t;
  return t;
}

int c_pthread_mutex_init(uint64_t* mutex, const void* /*attr*/) {
  noteCompatCall("pthread_mutex_init");
  if (!mutex) return EINVAL;
  MutexTable& t = mutexTable();
  std::lock_guard<std::mutex> l(t.table);
  // If slot already has an initialized mutex, destroy it first.
  auto it = t.slots.find(*mutex);
  if (it != t.slots.end() && it->second && it->second->initialized) {
    pthread_mutex_destroy(&it->second->mtx);
    it->second->initialized = false;
  }
  uint64_t id = (*mutex == 0) ? t.nextId++ : *mutex;
  auto& entry = t.slots[id];
  if (!entry) entry = std::make_unique<HostMutex>();
  pthread_mutexattr_t ma;
  pthread_mutexattr_init(&ma);
  pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_DEFAULT);
  int rc = pthread_mutex_init(&entry->mtx, &ma);
  pthread_mutexattr_destroy(&ma);
  if (rc == 0) entry->initialized = true;
  *mutex = id;
  return rc;
}

int c_pthread_mutex_destroy(uint64_t* mutex) {
  noteCompatCall("pthread_mutex_destroy");
  if (!mutex || *mutex == 0) return EINVAL;
  MutexTable& t = mutexTable();
  std::lock_guard<std::mutex> l(t.table);
  auto it = t.slots.find(*mutex);
  if (it == t.slots.end() || !it->second || !it->second->initialized) return EINVAL;
  int rc = pthread_mutex_destroy(&it->second->mtx);
  it->second->initialized = false;
  // Leave the slot in the map in case the guest uses the same storage again (it will re-init),
  // but free memory by clearing the unique_ptr if it knows it's done. Most callers zero the
  // slot afterwards anyway.
  return rc;
}

int c_pthread_mutex_lock(uint64_t* mutex) {
  if (!mutex) return EINVAL;
  MutexTable& t = mutexTable();
  // Lazy init for static PTHREAD_MUTEX_INITIALIZER-style locks (slot still 0 or not yet in table).
  {
    std::lock_guard<std::mutex> l(t.table);
    if (*mutex == 0 || t.slots.find(*mutex) == t.slots.end() ||
        !t.slots[*mutex] || !t.slots[*mutex]->initialized) {
      // Drop the lock to avoid double locking while init runs, by using c_pthread_mutex_init path.
      // We can call init inline under table lock since init also takes the lock -- so restructure:
      // release, call init, re-acquire.
    } else {
      HostMutex* hm = t.slots[*mutex].get();
      return pthread_mutex_lock(&hm->mtx);
    }
  }
  c_pthread_mutex_init(mutex, nullptr);
  // Re-fetch after init.
  std::lock_guard<std::mutex> l(t.table);
  auto it = t.slots.find(*mutex);
  if (it == t.slots.end() || !it->second) return EINVAL;
  return pthread_mutex_lock(&it->second->mtx);
}

int c_pthread_mutex_unlock(uint64_t* mutex) {
  if (!mutex) return EINVAL;
  MutexTable& t = mutexTable();
  std::lock_guard<std::mutex> l(t.table);
  auto it = t.slots.find(*mutex);
  if (it == t.slots.end() || !it->second || !it->second->initialized) return EINVAL;
  return pthread_mutex_unlock(&it->second->mtx);
}

int c_pthread_mutex_trylock(uint64_t* mutex) {
  if (!mutex) return EINVAL;
  MutexTable& t = mutexTable();
  std::lock_guard<std::mutex> l(t.table);
  auto it = t.slots.find(*mutex);
  if (it == t.slots.end() || !it->second || !it->second->initialized) return EINVAL;
  return pthread_mutex_trylock(&it->second->mtx);
}

// --- Timing -------------------------------------------------------------------------------
int c_gettimeofday(void* tv, void* tz) {
  noteCompatCall("gettimeofday");
  return ::gettimeofday(static_cast<struct timeval*>(tv), static_cast<struct timezone*>(tz));
}

// --- libc++ std::thread::hardware_concurrency() -------------------------------------------
// __ZNSt3__16thread20hardware_concurrencyEv -> returns unsigned int
uint32_t c_cxx_thread_hardware_concurrency() {
  noteCompatCall("std::thread::hardware_concurrency");
  unsigned int n = std::thread::hardware_concurrency();
  if (n == 0) n = 4;  // sensible fallback when the host reports 0
  return static_cast<uint32_t>(n);
}

// --- libc++ std::mutex destructor: __ZNSt3__15mutexD1Ev -----------------------------------
// Apple libc++'s std::mutex holds a pthread_mutex_t internally; the guest's destructor calls
// this symbol. We need to look up which host mutex backs the guest storage address and call
// pthread_mutex_destroy on it.
void c_cxx_mutex_d1(void* guestMutexPtr) {
  noteCompatCall("std::mutex::~mutex()");
  if (!guestMutexPtr) return;
  // The guest mutex is a 64-bit (or 40/48-byte) struct; its first word is the __m_ field
  // containing a pointer to (or an inline representation of) the underlying pthread_mutex_t.
  // In Apple's libc++ on ARM64, std::__1::mutex contains a pthread_mutex_t sized object that
  // stores a 64-bit signature/pointer. We treat the first 8 bytes as our mutex-table ID.
  uint64_t id = *reinterpret_cast<uint64_t*>(guestMutexPtr);
  if (id == 0) return;
  MutexTable& t = mutexTable();
  std::lock_guard<std::mutex> l(t.table);
  auto it = t.slots.find(id);
  if (it != t.slots.end() && it->second && it->second->initialized) {
    pthread_mutex_destroy(&it->second->mtx);
    it->second->initialized = false;
    // Leave the id stored so a subsequent re-init reuses the slot; zeroing here would make
    // re-init allocate a new id, which is also fine, but Apple code frequently in-place
    // constructs/destroys mutexes, so reuse is safer.
  }
}

// --- libc++ __shared_weak_count::__release_shared() ---------------------------------------
// Apple libc++ ABI layout for __shared_weak_count:
//   offset  0: const void* __vtable;       (8 bytes)
//   offset  8: long __shared_owners_;       (8 bytes, -1 == shared-only sentinel in older ABI;
//                                            signed; starts at 1; increments; decrement to 0 destroys)
//   offset 16: long __weak_owners_;         (8 bytes)
// __release_shared(): atomically decrement __shared_owners_; when it hits 0 call __on_zero_shared()
//   (virtual at vtable+16 on 64-bit -- but for simplicity we invoke the dtor stored as a function
//   pointer field, or treat the object header correctly). The simplest ABI-safe approach that
//   matches Apple's libc++ layout (and works with dlsym'd libc++ on the host) is to perform the
//   atomic decrement and call __release_weak() when the shared count hits zero.
struct SharedWeakCountLayout {
  const void* vtable;                 // 0
  std::atomic<int64_t> shared_owners; // 8
  std::atomic<int64_t> weak_owners;   // 16
};

bool c_cxx_shared_weak_count_release_shared(void* self) {
  noteCompatCall("std::__shared_weak_count::__release_shared()");
  if (!self) return false;

  // Forward to a host-side implementation when it's ABI-compatible. On Android with NDK libc++,
  // dlsym resolves __ZNSt3__119__shared_weak_count16__release_sharedEv directly, and passing
  // through honors the virtual __on_zero_shared / __on_zero_shared_weak without our hardcoding
  // vtable offsets.
  using HostReleaseSharedFn = bool (*)(void*);
  static HostReleaseSharedFn hostFn = [] {
    void* p = dlsym(RTLD_DEFAULT,
        "__ZNSt3__119__shared_weak_count16__release_sharedEv");
    if (!p) p = dlsym(RTLD_DEFAULT, "_ZNSt3__119__shared_weak_count16__release_sharedEv");
    return reinterpret_cast<HostReleaseSharedFn>(p);
  }();
  auto* cnt = static_cast<SharedWeakCountLayout*>(self);
  uintptr_t selfAddr = reinterpret_cast<uintptr_t>(self);
  uintptr_t vtAddr = reinterpret_cast<uintptr_t>(cnt->vtable);
  constexpr uintptr_t kLo = 0x100000ull;
  constexpr uintptr_t kHi = 0x00007FFFFFFFFFFFull;
  if (hostFn && selfAddr > kLo && selfAddr < kHi && vtAddr > kLo && vtAddr < kHi) {
    return hostFn(self);
  }

  // Manual refcounting path: used when the control block lives in guest memory (vtable is a
  // guest address that cannot be called directly from host) or when no host impl exists
  // (host-tests build).
  int64_t prev = cnt->shared_owners.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (prev != 0) return false;

  // Shared count dropped to 0: release the implicit weak reference holding the control block.
  int64_t wprev = cnt->weak_owners.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (wprev == 0) {
    // libc++ allocates control blocks with ::operator new; pair it with free/delete.
    std::free(self);
  }
  return true;
}

template <class F> uint64_t addr(F* f) { return reinterpret_cast<uint64_t>(f); }
}  // namespace

// Doctrine for this file: a symbol is registered here only when the guest-visible ABI is
// identical to the host function it points at, or when a written shim can honestly reproduce
// the documented behaviour. Nothing is stubbed to "succeed": anything we cannot back is left
// unregistered, and the relinker turns it into a named BRK trap that reports itself.

// _exit/_abort end the current run: abortGuest() unwinds back into runtime::run(), which
// reports the exit code, so the guest stops at the same point it would on iOS.
void appendGuestOutput(const std::string& s) {
  std::lock_guard<std::mutex> l(g_outMutex);
  g_out += s;
#ifdef __ANDROID__
  __android_log_write(ANDROID_LOG_INFO, "RadekiGuest", s.c_str());
#endif
}
std::string takeGuestOutput() {
  std::lock_guard<std::mutex> l(g_outMutex);
  std::string r;
  r.swap(g_out);
  return r;
}

relinker::CompatRegistry makeCompatRegistry() {
  ensureSandboxDirectories();
  relinker::CompatRegistry r;
  r.add("_puts", addr(c_puts));
  r.add("_putchar", addr(c_putchar));
  r.add("_exit", addr(c_exit));
  r.add("__exit", addr(c_immediate_exit));
  r.add("_abort", addr(c_abort));
  r.add("___stack_chk_fail", addr(c_stack_chk_fail));
  r.add("___stack_chk_guard", reinterpret_cast<uint64_t>(&g_stackGuard));  // data symbol
  r.add("___memcpy_chk", addr(c_memcpy_chk));
  r.add("___memset_chk", addr(c_memset_chk));
  r.add("_abs", addr(c_abs));
  r.add("_strchr", addr(c_strchr));
  // Plain libc functions with identical AArch64 calling convention.
  r.add("_malloc", addr(+[](size_t n) -> void* { noteCompatCall("malloc"); return std::malloc(n); }));
  r.add("_free", addr(&std::free));
  r.add("_calloc", addr(&std::calloc));
  r.add("_realloc", addr(&std::realloc));
  r.add("_memcpy", addr(&std::memcpy));
  r.add("_memmove", addr(&std::memmove));
  r.add("_memset", addr(&std::memset));
  r.add("_memcmp", addr(&std::memcmp));
  r.add("_strlen", addr(&std::strlen));
  r.add("_strcmp", addr(&std::strcmp));
  r.add("_strncmp", addr(&std::strncmp));
  r.add("_strcpy", addr(&std::strcpy));
  r.add("_strncpy", addr(&std::strncpy));
  r.add("_strdup", addr(&::strdup));
  r.add("_atoi", addr(&std::atoi));
  r.add("_usleep", addr(&::usleep));
  r.add("_sleep", addr(&::sleep));
  r.add("_getpid", addr(&::getpid));
  r.add("_time", addr(&std::time));
  r.add("_strerror", addr(&std::strerror));
  r.add("_strcat", addr(&std::strcat));
  r.add("_strncat", addr(&std::strncat));
  r.add("_strstr", addr(static_cast<char* (*)(char*, const char*)>(&std::strstr)));
  r.add("_strrchr", addr(static_cast<char* (*)(char*, int)>(&std::strrchr)));
  r.add("_strtol", addr(&std::strtol));
  r.add("_strtoul", addr(&std::strtoul));
  r.add("_atol", addr(&std::atol));
  // Sandboxed C/POSIX filesystem interceptors (rewriting iOS paths into /storage/emulated/0/RadekiOSNative/sandbox/).
  r.add("_open", addr(c_open));
  r.add("_close", addr(&::close));
  r.add("_read", addr(&::read));
  r.add("_write", addr(&::write));
  r.add("_lseek", addr(&::lseek));
  r.add("_fsync", addr(&::fsync));
  r.add("_ftruncate", addr(&::ftruncate));
  r.add("_fopen", addr(c_fopen));
  r.add("_fclose", addr(&std::fclose));
  r.add("_fread", addr(&std::fread));
  r.add("_fwrite", addr(&std::fwrite));
  r.add("_fseek", addr(&std::fseek));
  r.add("_ftell", addr(&std::ftell));
  r.add("_fflush", addr(&std::fflush));
  r.add("_feof", addr(&std::feof));
  r.add("_ferror", addr(&std::ferror));
  r.add("_fgets", addr(&std::fgets));
  r.add("_fputs", addr(&std::fputs));
  r.add("_fileno", addr(&::fileno));
  r.add("_stat", addr(c_stat));
  r.add("_stat64", addr(c_stat));
  r.add("_lstat", addr(c_lstat));
  r.add("_fstat", addr(c_fstat));
  r.add("_mkdir", addr(c_mkdir));
  r.add("_access", addr(c_access));
  r.add("_chdir", addr(c_chdir));
  r.add("_getcwd", addr(c_getcwd));
  r.add("_unlink", addr(c_unlink));
  r.add("_remove", addr(c_remove));
  r.add("_rmdir", addr(c_rmdir));
  r.add("_rename", addr(c_rename));
  r.add("_opendir", addr(c_opendir));
  r.add("_readdir", addr(&::readdir));
  r.add("_closedir", addr(&::closedir));
  // Foundation sandbox path methods (also registered in registerFrameworkStubs).
  r.add("_NSHomeDirectory", addr(guestNSHomeDirectory),
        {compat::SymbolClass::CompatibilityShim, "Foundation", "sandbox_path"});
  r.add("_NSTemporaryDirectory", addr(guestNSTemporaryDirectory),
        {compat::SymbolClass::CompatibilityShim, "Foundation", "sandbox_path"});
  r.add("_NSSearchPathForDirectoriesInDomains", addr(guestNSSearchPathForDirectoriesInDomains),
        {compat::SymbolClass::CompatibilityShim, "Foundation", "sandbox_path"});
  // pthread subset: trampolines where ABI layout differs from host (pthread_create, mutex table),
  // plus TLS key and specific helpers forwarded directly to host pthread.
  r.add("_pthread_create", addr(c_pthread_create));
  r.add("_pthread_join", addr(c_pthread_join));
  r.add("_pthread_self", addr(c_pthread_self));
  r.add("_pthread_key_create", addr(c_pthread_key_create));
  r.add("_pthread_key_delete", addr(c_pthread_key_delete));
  r.add("_pthread_getspecific", addr(c_pthread_getspecific));
  r.add("_pthread_setspecific", addr(c_pthread_setspecific));
  r.add("_pthread_mutex_init", addr(c_pthread_mutex_init));
  r.add("_pthread_mutex_destroy", addr(c_pthread_mutex_destroy));
  r.add("_pthread_mutex_lock", addr(c_pthread_mutex_lock));
  r.add("_pthread_mutex_unlock", addr(c_pthread_mutex_unlock));
  r.add("_pthread_mutex_trylock", addr(c_pthread_mutex_trylock));
  // Timing
  r.add("_gettimeofday", addr(c_gettimeofday));
  // libc++ symbols that the guest imports from libc++.1.dylib: std::thread::hardware_concurrency,
  // std::mutex destructor, and __shared_weak_count::__release_shared (real refcounting so
  // shared_ptr-managed memory is freed instead of leaked).
  r.add("__ZNSt3__16thread20hardware_concurrencyEv", addr(c_cxx_thread_hardware_concurrency),
        {compat::SymbolClass::CompatibilityShim, "libc++", "thread_hardware_concurrency"});
  r.add("__ZNSt3__15mutexD1Ev", addr(c_cxx_mutex_d1),
        {compat::SymbolClass::CompatibilityShim, "libc++", "mutex_dtor"});
  r.add("__ZNSt3__15mutexD2Ev", addr(c_cxx_mutex_d1),
        {compat::SymbolClass::CompatibilityShim, "libc++", "mutex_base_dtor"});
  r.add("__ZNSt3__119__shared_weak_count16__release_sharedEv",
        addr(c_cxx_shared_weak_count_release_shared),
        {compat::SymbolClass::CompatibilityShim, "libc++", "shared_weak_count_release_shared"});
  // libm: ordinary C functions taking and returning doubles, so the AArch64 ABI matches.
  r.add("_fabs", addr(static_cast<double (*)(double)>(&std::fabs)));
  r.add("_sqrt", addr(static_cast<double (*)(double)>(&std::sqrt)));
  r.add("_pow", addr(static_cast<double (*)(double, double)>(&std::pow)));
  r.add("_fmod", addr(static_cast<double (*)(double, double)>(&std::fmod)));
  r.add("_sin", addr(static_cast<double (*)(double)>(&std::sin)));
  r.add("_cos", addr(static_cast<double (*)(double)>(&std::cos)));
  r.add("_floor", addr(static_cast<double (*)(double)>(&std::floor)));
  r.add("_ceil", addr(static_cast<double (*)(double)>(&std::ceil)));
  r.add("_log", addr(static_cast<double (*)(double)>(&std::log)));
  r.add("_exp", addr(static_cast<double (*)(double)>(&std::exp)));
  addCxxCompat(r);
  // libc++.1.dylib -> host C++ runtime forwarding table (basic_string wrappers, ios_base::Init,
  // std::string::npos...). Deliberately NOT here: framework dummy classes/stubs (see
  // registerFrameworkStubs) and C++ exception symbols (left unbound on purpose).
  addCxxForwarding(r);
  return r;
}

}  // namespace radeki::runtime

// libSystem: paths, stdio, file descriptors, directories, stat, mmap.
#include "libc/Files.h"
#include "Hle.h"
#include "Log.h"
#include "Memory.h"
#include "Runtime.h"
#include "libc/Format.h"
#include "platform/Platform.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;
using hle::arg;
using hle::ret;

namespace files {
// iOS 4 layout: the bundle lives inside the app's home directory.
const char *const kBundlePath = "/var/mobile/Applications/5C3A39B1-6A3B-4C2E-9D5A-0C1A2B3C4D5E/Forlorn.app";
const char *const kHomePath = "/var/mobile/Applications/5C3A39B1-6A3B-4C2E-9D5A-0C1A2B3C4D5E";
namespace {
std::string g_sandbox;
}
void setSandboxRoot(const std::string &path) {
    g_sandbox = path;
    for (const char *sub : {"Documents", "Library/Caches", "Library/Preferences", "tmp"}) fs::create_directories(fs::path(path) / sub);
}
const std::string &sandboxRoot() { return g_sandbox; }
std::string hostPath(const std::string &guest) {
    const std::string bundle = kBundlePath, home = kHomePath;
    auto under = [&](const std::string &prefix, std::string &rest) {
        if (guest.compare(0, prefix.size(), prefix) != 0) return false;
        if (guest.size() > prefix.size() && guest[prefix.size()] != '/') return false;
        rest = guest.substr(prefix.size());
        if (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
        return true;
    };
    std::string rest;
    if (under(bundle, rest)) return (fs::path(runtime::bundlePath()) / fs::u8path(rest)).string();
    if (under(home, rest)) return (fs::path(g_sandbox) / fs::u8path(rest)).string();
    if (under("/tmp", rest) || under("/private/tmp", rest)) return (fs::path(g_sandbox) / "tmp" / fs::u8path(rest)).string();
    // Relative paths: relative to the bundle (the process's working directory on iOS is "/", so this is a guess that
    // matches how the game opens resources by relative name).
    if (guest.empty()) return (fs::path(g_sandbox) / "root").string();
    if (guest[0] != '/') return (fs::path(runtime::bundlePath()) / fs::u8path(guest)).string();
    return (fs::path(g_sandbox) / "root" / fs::u8path(guest.substr(1))).string();
}
bool readWholeFile(const std::string &guestPath, std::string &out) {
    if (guestPath.empty()) return false;
    std::ifstream in(fs::u8path(hostPath(guestPath)), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}
} // namespace files

namespace {
// errno: one int per thread in guest memory.
thread_local std::uint64_t t_errno = 0;
std::uint64_t errnoAddress() {
    if (!t_errno) t_errno = mem::calloc(16);
    return t_errno;
}
void setErrno(int e) { mem::w32(errnoAddress(), std::uint32_t(e)); }
// Darwin errno values match the host's (MSVC, Linux) up to 34; above, the ones that matter are mapped (EAGAIN /
// EWOULDBLOCK, the same value on Linux, are 35 on Darwin).
int darwinErrno(int e) {
    if (e == EAGAIN || e == EWOULDBLOCK) return 35;
    if (e == EINPROGRESS) return 36;
    return e;
}

// FILE objects: guest memory (Darwin's 32-bit FILE is 88 bytes), mapped to host FILE*.
std::mutex g_filesLock;
std::map<std::uint64_t, FILE *> g_files;
std::uint64_t newGuestFile(FILE *host) {
    const auto f = mem::calloc(152);
    std::lock_guard lock(g_filesLock);
    g_files[f] = host;
    return f;
}
FILE *hostFile(std::uint64_t guest) {
    std::lock_guard lock(g_filesLock);
    auto found = g_files.find(guest);
    return found == g_files.end() ? nullptr : found->second;
}
std::uint64_t g_stdin = 0, g_stdout = 0, g_stderr = 0;
std::uint64_t stdFile(std::uint64_t &slot, FILE *host) {
    if (!slot) {
        const auto pointer = mem::hleAlloc(4);
        mem::wp(pointer, newGuestFile(host));
        slot = pointer;
    }
    return slot;
}
} // namespace

HLE(__error) { ret(errnoAddress()); }
static hle::DataRegistrar d_stdout("___stdoutp", [] { return stdFile(g_stdout, stdout); });
static hle::DataRegistrar d_stderr("___stderrp", [] { return stdFile(g_stderr, stderr); });
static hle::DataRegistrar d_stdin("___stdinp", [] { return stdFile(g_stdin, stdin); });

// --- stdio ---
HLE(fopen) {
    const std::string path = mem::cstring(arg(0)), mode = mem::cstring(arg(1));
    const auto host = files::hostPath(path);
    if (mode.find('w') != std::string::npos || mode.find('a') != std::string::npos) fs::create_directories(fs::u8path(host).parent_path());
    std::string m = mode;
    if (m.find('b') == std::string::npos) m += 'b';
    FILE *f = platform::openFile(host, m.c_str());
    hlog("file", "fopen %s mode=%s%s", path.c_str(), mode.c_str(), f ? "" : " (not found)");
    if (!f) { setErrno(2); return ret(0); }
    ret(newGuestFile(f));
}
HLE(fclose) {
    FILE *f = hostFile(arg(0));
    if (f && f != stdout && f != stderr && f != stdin) std::fclose(f);
    std::lock_guard lock(g_filesLock);
    g_files.erase(arg(0));
    ret(0);
}
HLE(fread) {
    FILE *f = hostFile(arg(3));
    ret(f ? std::fread(mem::base() + arg(0), std::size_t(arg(1)), std::size_t(arg(2)), f) : 0);
}
HLE(fwrite) {
    FILE *f = hostFile(arg(3));
    ret(f ? std::fwrite(mem::base() + arg(0), std::size_t(arg(1)), std::size_t(arg(2)), f) : 0);
}
// fseek/ftell use a 32-bit long; fseeko/ftello a 64-bit off_t (two words).
HLE(fseek) { FILE *f = hostFile(arg(0)); ret(f ? std::uint64_t(std::int64_t(platform::seekFile(f, hle::argInt(1), int(arg(2))))) : ~0ull); }
HLE(fseeko) { FILE *f = hostFile(arg(0)); ret(f ? std::uint64_t(std::int64_t(platform::seekFile(f, hle::arg64(1), int(arg(3))))) : ~0ull); }
HLE(ftell) { FILE *f = hostFile(arg(0)); ret(f ? std::uint64_t(platform::tellFile(f)) : ~0ull); }
HLE(ftello) { FILE *f = hostFile(arg(0)); hle::ret64(f ? std::uint64_t(platform::tellFile(f)) : ~0ull); }
HLE(rewind) { if (FILE *f = hostFile(arg(0))) std::rewind(f); }
HLE(feof) { FILE *f = hostFile(arg(0)); ret(f ? std::feof(f) != 0 : 1); }
HLE(ferror) { FILE *f = hostFile(arg(0)); ret(f ? std::ferror(f) != 0 : 1); }
HLE(fflush) { if (FILE *f = hostFile(arg(0))) std::fflush(f); ret(0); }
HLE(fileno) { FILE *f = hostFile(arg(0)); ret(f ? std::uint64_t(std::int64_t(platform::fileDescriptor(f))) : ~0ull); }
HLE(fgetc) { FILE *f = hostFile(arg(0)); ret(f ? std::uint64_t(std::int64_t(std::fgetc(f))) : ~0ull); }
HLE(getc) { FILE *f = hostFile(arg(0)); ret(f ? std::uint64_t(std::int64_t(std::fgetc(f))) : ~0ull); }
HLE(fgets) {
    FILE *f = hostFile(arg(2));
    if (!f) return ret(0);
    ret(std::fgets(reinterpret_cast<char *>(mem::base() + arg(0)), int(arg(1)), f) ? arg(0) : 0);
}
HLE(fputc) { FILE *f = hostFile(arg(1)); ret(f ? std::uint64_t(std::fputc(int(arg(0)), f)) : ~0ull); }
HLE(putc) { FILE *f = hostFile(arg(1)); ret(f ? std::uint64_t(std::fputc(int(arg(0)), f)) : ~0ull); }
HLE(fputs) { FILE *f = hostFile(arg(1)); ret(f ? std::uint64_t(std::fputs(mem::cstring(arg(0)).c_str(), f)) : ~0ull); }
HLE(puts) { hlog("stdout", "%s", mem::cstring(arg(0)).c_str()); ret(1); }
HLE(putchar) { std::fputc(int(arg(0)), stdout); ret(arg(0)); }
HLE(printf) {
    const auto text = libc::format(mem::cstring(arg(0)), libc::ArgSource::fromArgs(1));
    hlog("stdout", "%s", text.c_str());
    ret(text.size());
}
HLE(fprintf) {
    const auto text = libc::format(mem::cstring(arg(1)), libc::ArgSource::fromArgs(2));
    FILE *f = hostFile(arg(0));
    if (f == stdout || f == stderr) hlog("stdout", "%s", text.c_str());
    else if (f) std::fwrite(text.data(), 1, text.size(), f);
    ret(text.size());
}
HLE(vfprintf) {
    const auto text = libc::format(mem::cstring(arg(1)), libc::ArgSource::fromPointer(arg(2)));
    FILE *f = hostFile(arg(0));
    if (f == stdout || f == stderr) hlog("stdout", "%s", text.c_str());
    else if (f) std::fwrite(text.data(), 1, text.size(), f);
    ret(text.size());
}
static void writeFormatted(std::uint64_t buffer, std::uint64_t size, const std::string &text) {
    if (size) {
        const auto n = std::min<std::uint64_t>(text.size(), size - 1);
        std::memcpy(mem::base() + buffer, text.data(), std::size_t(n));
        mem::base()[buffer + n] = 0;
    }
    hle::ret(text.size());
}
HLE(sprintf) { const auto t = libc::format(mem::cstring(arg(1)), libc::ArgSource::fromArgs(2)); writeFormatted(arg(0), t.size() + 1, t); }
HLE(snprintf) { writeFormatted(arg(0), arg(1), libc::format(mem::cstring(arg(2)), libc::ArgSource::fromArgs(3))); }
HLE(__snprintf_chk) { writeFormatted(arg(0), arg(1), libc::format(mem::cstring(arg(4)), libc::ArgSource::fromArgs(5))); }
HLE(__sprintf_chk) { const auto t = libc::format(mem::cstring(arg(3)), libc::ArgSource::fromArgs(4)); writeFormatted(arg(0), t.size() + 1, t); }
HLE(vsnprintf) { writeFormatted(arg(0), arg(1), libc::format(mem::cstring(arg(2)), libc::ArgSource::fromPointer(arg(3)))); }
HLE(vsprintf) { const auto t = libc::format(mem::cstring(arg(1)), libc::ArgSource::fromPointer(arg(2))); writeFormatted(arg(0), t.size() + 1, t); }
HLE(__vsnprintf_chk) { writeFormatted(arg(0), arg(1), libc::format(mem::cstring(arg(4)), libc::ArgSource::fromPointer(arg(5)))); }
HLE(vasprintf) {
    const auto t = libc::format(mem::cstring(arg(1)), libc::ArgSource::fromPointer(arg(2)));
    mem::wp(arg(0), mem::heapString(t));
    ret(t.size());
}
HLE(asprintf) {
    const auto t = libc::format(mem::cstring(arg(1)), libc::ArgSource::fromArgs(2));
    mem::wp(arg(0), mem::heapString(t));
    ret(t.size());
}
// sscanf: the game's uses are simple ("%d", "%x", "%lx-%lx ...", "%f"); arguments are pointers in stack slots.
HLE(sscanf) {
    const std::string input = mem::cstring(arg(0)), fmt = mem::cstring(arg(1));
    auto args = libc::ArgSource::fromArgs(2);
    std::size_t in = 0;
    int assigned = 0;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(fmt[i]))) { while (in < input.size() && std::isspace(static_cast<unsigned char>(input[in]))) ++in; continue; }
        if (fmt[i] != '%') { if (in < input.size() && input[in] == fmt[i]) { ++in; continue; } break; }
        ++i;
        bool skip = false;
        if (fmt[i] == '*') { skip = true; ++i; }
        int width = 0;
        while (std::isdigit(static_cast<unsigned char>(fmt[i]))) width = width * 10 + (fmt[i++] - '0');
        std::string length;
        while (std::strchr("hlLqjz", fmt[i])) length.push_back(fmt[i++]);
        const char conv = fmt[i];
        if (conv != 'c') while (in < input.size() && std::isspace(static_cast<unsigned char>(input[in]))) ++in;
        if (in >= input.size()) break;
        const char *start = input.c_str() + in;
        char *end = nullptr;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            const int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            const unsigned long long v = conv == 'd' || conv == 'i' ? (unsigned long long)std::strtoll(start, &end, base) : std::strtoull(start, &end, base);
            if (end == start) break;
            in += std::size_t(end - start);
            if (!skip) {
                const auto dest = args.next();
                if (length == "ll" || length == "q" || length == "j") mem::write<std::uint64_t>(dest, v);
                else if (length == "h") mem::write<std::uint16_t>(dest, std::uint16_t(v));
                else if (length == "hh") mem::write<std::uint8_t>(dest, std::uint8_t(v));
                else mem::w32(dest, std::uint32_t(v));
                ++assigned;
            }
        } else if (conv == 'f' || conv == 'e' || conv == 'g') {
            const double v = std::strtod(start, &end);
            if (end == start) break;
            in += std::size_t(end - start);
            if (!skip) {
                const auto dest = args.next();
                if (length == "l") mem::write(dest, v); else mem::write(dest, float(v));
                ++assigned;
            }
        } else if (conv == 's') {
            std::size_t n = 0;
            while (in + n < input.size() && !std::isspace(static_cast<unsigned char>(input[in + n])) && (!width || int(n) < width)) ++n;
            if (!skip) {
                const auto dest = args.next();
                std::memcpy(mem::base() + dest, input.data() + in, n);
                mem::base()[dest + n] = 0;
                ++assigned;
            }
            in += n;
        } else if (conv == 'c') {
            if (!skip) { mem::base()[args.next()] = std::uint8_t(input[in]); ++assigned; }
            ++in;
        } else break;
    }
    ret(std::uint64_t(assigned));
}
HLE(remove) { std::error_code ec; ret(fs::remove(fs::u8path(files::hostPath(mem::cstring(arg(0)))), ec) ? 0 : ~0ull); }
HLE(unlink) { std::error_code ec; ret(fs::remove(fs::u8path(files::hostPath(mem::cstring(arg(0)))), ec) ? 0 : ~0ull); }
HLE(rename) {
    std::error_code ec;
    fs::rename(fs::u8path(files::hostPath(mem::cstring(arg(0)))), fs::u8path(files::hostPath(mem::cstring(arg(1)))), ec);
    ret(ec ? ~0ull : 0);
}
HLE(mkdir) {
    std::error_code ec;
    const bool made = fs::create_directory(fs::u8path(files::hostPath(mem::cstring(arg(0)))), ec);
    if (!made) setErrno(17);
    ret(made ? 0 : ~0ull);
}
HLE(rmdir) { std::error_code ec; ret(fs::remove(fs::u8path(files::hostPath(mem::cstring(arg(0)))), ec) ? 0 : ~0ull); }
HLE(access) { ret(fs::exists(fs::u8path(files::hostPath(mem::cstring(arg(0))))) ? 0 : ~0ull); }
HLE(mkstemp) {
    std::string templ = mem::cstring(arg(0));
    for (auto &c : templ) if (c == 'X') c = char('a' + std::rand() % 26);
    std::memcpy(mem::base() + arg(0), templ.data(), templ.size());
    const auto host = files::hostPath(templ);
    ret(std::uint64_t(std::int64_t(platform::createFile(host))));
}
HLE(setxattr) { ret(0); }

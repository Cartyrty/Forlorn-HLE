// platform/Platform.h on Linux (POSIX and SDL2).
#include "platform/Platform.h"
#include <SDL.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace platform {
namespace {
// Committed pages of the (single) reserved range, one bit per 4 KB page: Linux has no VirtualQuery.
std::uint8_t *g_reserved = nullptr;
std::uint64_t g_reservedSize = 0;
std::vector<std::uint64_t> g_committed;
std::mutex g_memoryLock;
std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

void *reserveAddressSpace(std::uint64_t size) {
    void *p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    std::lock_guard lock(g_memoryLock);
    g_reserved = static_cast<std::uint8_t *>(p);
    g_reservedSize = size;
    g_committed.assign(std::size_t((size / 4096 + 63) / 64), 0);
    return p;
}
bool commitMemory(void *address, std::uint64_t size) {
    if (mprotect(address, size, PROT_READ | PROT_WRITE) != 0) return false;
    std::lock_guard lock(g_memoryLock);
    const auto first = std::uint64_t(static_cast<std::uint8_t *>(address) - g_reserved) / 4096;
    for (std::uint64_t page = first; page < first + (size + 4095) / 4096; ++page) g_committed[page / 64] |= 1ull << (page % 64);
    return true;
}
bool isCommitted(const void *address) {
    std::lock_guard lock(g_memoryLock);
    const auto *p = static_cast<const std::uint8_t *>(address);
    if (p < g_reserved || p >= g_reserved + g_reservedSize) return false;
    const auto page = std::uint64_t(p - g_reserved) / 4096;
    return (g_committed[page / 64] >> (page % 64)) & 1;
}

std::uint32_t currentThreadId() { return std::uint32_t(syscall(SYS_gettid)); }
std::uint64_t milliseconds() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// SDL_WaitEventTimeout with no event structure waits for input without taking it off the queue.
void waitForEvents(int ms) {
    if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_WaitEventTimeout(nullptr, std::max(0, ms));
    else std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, ms)));
}

void *glProc(const char *name) { return SDL_GL_GetProcAddress(name); }

// Font files by name in the usual font folders (searched once, recursively).
std::string findFont(const std::vector<std::string> &fileNames) {
    static std::once_flag once;
    static std::map<std::string, std::string> index; // lower-case file name -> path
    std::call_once(once, [] {
        std::vector<std::filesystem::path> roots = {"/usr/share/fonts", "/usr/local/share/fonts"};
        if (const char *home = std::getenv("HOME")) {
            roots.push_back(std::filesystem::path(home) / ".fonts");
            roots.push_back(std::filesystem::path(home) / ".local/share/fonts");
        }
        for (const auto &root : roots) {
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec)) index.emplace(lower(it->path().filename().string()), it->path().string());
        }
    });
    for (const auto &name : fileNames)
        if (auto found = index.find(lower(name)); found != index.end()) return found->second;
    return {};
}

FILE *openFile(const std::string &utf8Path, const char *mode) { return std::fopen(utf8Path.c_str(), mode); }
int seekFile(FILE *file, std::int64_t offset, int whence) { return fseeko(file, off_t(offset), whence); }
std::int64_t tellFile(FILE *file) { return std::int64_t(ftello(file)); }
int fileDescriptor(FILE *file) { return fileno(file); }
int createFile(const std::string &utf8Path) { return open(utf8Path.c_str(), O_CREAT | O_RDWR, 0644); }
std::tm utcTime(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    return tm;
}
std::tm localTime(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm;
}
std::time_t fromUtcTime(std::tm t) { return timegm(&t); }
long utcOffsetSeconds() {
    tzset();
    return -timezone;
}

int showAlert(const std::string &title, const std::string &message, const std::vector<std::string> &labels) {
    std::vector<std::string> texts = labels;
    if (texts.empty()) texts.push_back("OK");
    std::vector<SDL_MessageBoxButtonData> buttons;
    for (std::size_t i = 0; i < texts.size(); ++i) buttons.push_back({i == 0 ? Uint32(SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) : 0u, int(i), texts[i].c_str()});
    SDL_MessageBoxData data{};
    data.flags = SDL_MESSAGEBOX_INFORMATION;
    data.window = SDL_GL_GetCurrentWindow();
    data.title = title.empty() ? "Forlorn" : title.c_str();
    data.message = message.c_str();
    data.numbuttons = int(buttons.size());
    data.buttons = buttons.data();
    int pressed = 0;
    if (SDL_ShowMessageBox(&data, &pressed) != 0 || pressed < 0) pressed = 0;
    return pressed;
}
} // namespace platform

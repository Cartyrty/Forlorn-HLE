// platform/Platform.h on Windows.
#include "platform/Platform.h"
#include <windows.h>
#include <commctrl.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace platform {
namespace {
std::wstring wide(const std::string &utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
    return out;
}
std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

void *reserveAddressSpace(std::uint64_t size) { return VirtualAlloc(nullptr, SIZE_T(size), MEM_RESERVE, PAGE_NOACCESS); }
bool commitMemory(void *address, std::uint64_t size) { return VirtualAlloc(address, SIZE_T(size), MEM_COMMIT, PAGE_READWRITE) != nullptr; }
bool isCommitted(const void *address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(address, &info, sizeof info) && info.State == MEM_COMMIT;
}

std::uint32_t currentThreadId() { return GetCurrentThreadId(); }
std::uint64_t milliseconds() { return GetTickCount64(); }
// MsgWaitForMultipleObjects returns early on window input.
void waitForEvents(int ms) { MsgWaitForMultipleObjects(0, nullptr, FALSE, DWORD(std::max(0, ms)), QS_ALLINPUT); }

void *glProc(const char *name) {
    void *p = reinterpret_cast<void *>(wglGetProcAddress(name));
    // wglGetProcAddress only knows extensions; OpenGL 1.1 functions come from opengl32.dll (it may also answer 1-3, -1).
    if (!p || p == reinterpret_cast<void *>(1) || p == reinterpret_cast<void *>(2) || p == reinterpret_cast<void *>(3) ||
        p == reinterpret_cast<void *>(-1)) {
        static HMODULE gl = LoadLibraryA("opengl32.dll");
        p = reinterpret_cast<void *>(GetProcAddress(gl, name));
    }
    return p;
}

std::string findFont(const std::vector<std::string> &fileNames) {
    char windows[MAX_PATH];
    GetWindowsDirectoryA(windows, MAX_PATH);
    const auto fonts = std::filesystem::path(windows) / "Fonts";
    for (const auto &name : fileNames) {
        std::error_code ec;
        if (std::filesystem::exists(fonts / name, ec)) return (fonts / name).string();
    }
    return {};
}

FILE *openFile(const std::string &utf8Path, const char *mode) { return _wfopen(wide(utf8Path).c_str(), wide(mode).c_str()); }
int seekFile(FILE *file, std::int64_t offset, int whence) { return _fseeki64(file, offset, whence); }
std::int64_t tellFile(FILE *file) { return _ftelli64(file); }
int fileDescriptor(FILE *file) { return _fileno(file); }
int createFile(const std::string &utf8Path) { return _wopen(wide(utf8Path).c_str(), _O_CREAT | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE); }
std::tm utcTime(std::time_t t) {
    std::tm tm{};
    gmtime_s(&tm, &t);
    return tm;
}
std::tm localTime(std::time_t t) {
    std::tm tm{};
    localtime_s(&tm, &t);
    return tm;
}
std::time_t fromUtcTime(std::tm t) { return _mkgmtime(&t); }
long utcOffsetSeconds() {
    long tz = 0;
    _get_timezone(&tz);
    return -tz;
}

// A task dialog (the message, one button per label); a plain message box if task dialogs are unavailable.
int showAlert(const std::string &title, const std::string &message, const std::vector<std::string> &labels) {
    std::vector<std::wstring> texts;
    for (const auto &l : labels) texts.push_back(wide(l));
    if (texts.empty()) texts.push_back(L"OK");
    std::vector<TASKDIALOG_BUTTON> buttons;
    for (std::size_t i = 0; i < texts.size(); ++i) buttons.push_back({int(100 + i), texts[i].c_str()});
    const auto heading = wide(title), body = wide(message);
    TASKDIALOGCONFIG config{sizeof config};
    config.hwndParent = GetActiveWindow();
    config.pszWindowTitle = L"Forlorn";
    config.pszMainInstruction = heading.c_str();
    config.pszContent = body.c_str();
    config.cButtons = UINT(buttons.size());
    config.pButtons = buttons.data();
    config.nDefaultButton = 100;
    int pressed = 100;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) {
        MessageBoxW(config.hwndParent, body.c_str(), heading.c_str(), MB_OK);
        pressed = 100;
    }
    return pressed - 100;
}
} // namespace platform

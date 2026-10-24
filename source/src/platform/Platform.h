#pragma once
// Operating-system services the emulator needs besides the window (Window.h), audio output (AudioOutput.h) and
// compressed-audio decoding (MediaDecode.h). Each is implemented once per host: platform/win32 (Windows API) and
// platform/sdl (Linux: SDL2 and POSIX).
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace platform {
// Address space: reserve inaccessible, commit read/write (zero-filled), query.
void *reserveAddressSpace(std::uint64_t size);
bool commitMemory(void *address, std::uint64_t size);
bool isCommitted(const void *address);

std::uint32_t currentThreadId();
std::uint64_t milliseconds(); // monotonic
// Sleeps up to `ms` milliseconds, returning early when window input arrives.
void waitForEvents(int ms);

// An OpenGL function of the current context (extensions and core functions past the platform's link library).
void *glProc(const char *name);

// The first of the font files (by file name, searched case-insensitively in the system's font folders), or "".
std::string findFont(const std::vector<std::string> &fileNames);

// stdio with UTF-8 paths and 64-bit offsets.
FILE *openFile(const std::string &utf8Path, const char *mode);
int seekFile(FILE *file, std::int64_t offset, int whence);
std::int64_t tellFile(FILE *file);
int fileDescriptor(FILE *file);
int createFile(const std::string &utf8Path); // O_CREAT | O_RDWR, a file descriptor or -1
std::tm utcTime(std::time_t t);
std::tm localTime(std::time_t t);
std::time_t fromUtcTime(std::tm t);  // timegm
long utcOffsetSeconds();             // local time minus UTC (standard time)

// A modal message box with one button per label (at least one); returns the index of the button pressed.
int showAlert(const std::string &title, const std::string &message, const std::vector<std::string> &buttons);
} // namespace platform

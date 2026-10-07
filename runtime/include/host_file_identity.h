#pragma once
#include <filesystem>
#include <string>
#include <sstream>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif
namespace PSXRecompV4 {
// A failed identity is never a cache hit. Change time protects restored mtimes.
inline bool host_file_identity(const std::filesystem::path& path, std::string& identity) {
    identity.clear();
    std::ostringstream out;
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    const bool ok = GetFileInformationByHandle(file, &info) &&
        GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof basic) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    CloseHandle(file);
    if (!ok) return false;
    out << "win1:" << info.dwVolumeSerialNumber << ':' << info.nFileIndexHigh << ':'
        << info.nFileIndexLow << ':' << info.nFileSizeHigh << ':' << info.nFileSizeLow
        << ':' << basic.LastWriteTime.QuadPart << ':' << basic.ChangeTime.QuadPart;
#else
    struct stat info{};
    if (stat(path.c_str(), &info) || !S_ISREG(info.st_mode)) return false;
    out << "posix1:" << info.st_dev << ':' << info.st_ino << ':' << info.st_size;
#if defined(__APPLE__)
    out << ':' << info.st_mtimespec.tv_sec << ':' << info.st_mtimespec.tv_nsec
        << ':' << info.st_ctimespec.tv_sec << ':' << info.st_ctimespec.tv_nsec;
#else
    out << ':' << info.st_mtim.tv_sec << ':' << info.st_mtim.tv_nsec
        << ':' << info.st_ctim.tv_sec << ':' << info.st_ctim.tv_nsec;
#endif
#endif
    identity = out.str();
    return true;
}
}

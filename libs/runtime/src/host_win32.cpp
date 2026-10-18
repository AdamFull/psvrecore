#include "psvrecore/runtime/host.hpp"

#include <windows.h>

#include <atomic>

namespace psvrecore::runtime::host {

namespace {

std::wstring wide(nx::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(utf8.data()), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(utf8.data()), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

FileError from_win32(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE: return FileError::NotFound;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return FileError::Exists;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_WRITE_PROTECT: return FileError::Access;
        case ERROR_DIR_NOT_EMPTY: return FileError::NotEmpty;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: return FileError::NoSpace;
        case ERROR_DIRECTORY: return FileError::NotDirectory;
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_PARAMETER: return FileError::Invalid;
        default: return FileError::Other;
    }
}

struct FaultRegion {
    std::atomic<uintptr_t> base{0};
    std::atomic<u64> size{0};
    std::atomic<FaultHandler> handler{nullptr};
    void* vectored = nullptr;
};
FaultRegion g_fault;

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const FaultHandler handler = g_fault.handler.load(std::memory_order_acquire);
    const uintptr_t base = g_fault.base.load(std::memory_order_relaxed);
    const uintptr_t address = static_cast<uintptr_t>(r->ExceptionInformation[1]);
    if (!handler || address < base || address - base >= g_fault.size.load(std::memory_order_relaxed))
        return EXCEPTION_CONTINUE_SEARCH;
    handler(address - base, r->ExceptionInformation[0] == 1);
    return EXCEPTION_CONTINUE_SEARCH;  // (the handler reports; the process then ends as for any access violation)
}

}  // namespace

bool decommit(void* address, usize size) {
    return VirtualFree(address, size, MEM_DECOMMIT) != 0;
}

void set_fault_region(void* base, u64 size, FaultHandler on_fault) {
    g_fault.handler.store(nullptr, std::memory_order_release);
    g_fault.base.store(reinterpret_cast<uintptr_t>(base), std::memory_order_relaxed);
    g_fault.size.store(size, std::memory_order_relaxed);
    g_fault.handler.store(on_fault, std::memory_order_release);
    if (on_fault && !g_fault.vectored) g_fault.vectored = AddVectoredExceptionHandler(1, on_exception);
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalid;
    }
    return *this;
}

File File::open(nx::string_view path, const OpenOptions& options, FileError* error) {
    DWORD access = 0;
    if (static_cast<u32>(options.mode) & static_cast<u32>(OpenMode::Read)) access |= GENERIC_READ;
    if (static_cast<u32>(options.mode) & static_cast<u32>(OpenMode::Write)) access |= GENERIC_WRITE;
    DWORD disposition = OPEN_EXISTING;
    if (options.create && options.exclusive) disposition = CREATE_NEW;
    else if (options.create && options.truncate) disposition = CREATE_ALWAYS;
    else if (options.create) disposition = OPEN_ALWAYS;
    else if (options.truncate) disposition = TRUNCATE_EXISTING;
    const HANDLE h = CreateFileW(wide(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    File f;
    if (h == INVALID_HANDLE_VALUE) {
        if (error) {
            const DWORD code = GetLastError();
            // A directory cannot be opened as a file.
            const DWORD attributes = GetFileAttributesW(wide(path).c_str());
            *error = (code == ERROR_ACCESS_DENIED && attributes != INVALID_FILE_ATTRIBUTES &&
                      (attributes & FILE_ATTRIBUTE_DIRECTORY))
                         ? FileError::IsDirectory
                         : from_win32(code);
        }
        return f;
    }
    f.handle_ = reinterpret_cast<intptr_t>(h);
    if (error) *error = FileError::None;
    return f;
}

void File::close() {
    if (handle_ != kInvalid) CloseHandle(reinterpret_cast<HANDLE>(handle_));
    handle_ = kInvalid;
}

i64 File::read_at(u64 offset, void* buffer, u64 size) const {
    u8* out = static_cast<u8*>(buffer);
    u64 done = 0;
    while (done < size) {
        OVERLAPPED o{};
        const u64 at = offset + done;
        o.Offset = static_cast<DWORD>(at);
        o.OffsetHigh = static_cast<DWORD>(at >> 32);
        const DWORD chunk = static_cast<DWORD>(size - done > 0x40000000u ? 0x40000000u : size - done);
        DWORD n = 0;
        if (!ReadFile(reinterpret_cast<HANDLE>(handle_), out + done, chunk, &n, &o)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return done ? static_cast<i64>(done) : -1;
        }
        done += n;
        if (n < chunk) break;
    }
    return static_cast<i64>(done);
}

i64 File::write_at(u64 offset, const void* buffer, u64 size) const {
    const u8* in = static_cast<const u8*>(buffer);
    u64 done = 0;
    while (done < size) {
        OVERLAPPED o{};
        const u64 at = offset + done;
        o.Offset = static_cast<DWORD>(at);
        o.OffsetHigh = static_cast<DWORD>(at >> 32);
        const DWORD chunk = static_cast<DWORD>(size - done > 0x40000000u ? 0x40000000u : size - done);
        DWORD n = 0;
        if (!WriteFile(reinterpret_cast<HANDLE>(handle_), in + done, chunk, &n, &o)) return done ? static_cast<i64>(done) : -1;
        done += n;
        if (n < chunk) break;
    }
    return static_cast<i64>(done);
}

i64 File::append(const void* buffer, u64 size) const {
    OVERLAPPED o{};
    o.Offset = 0xFFFFFFFFu;  // (with OffsetHigh: write at the end of the file)
    o.OffsetHigh = 0xFFFFFFFFu;
    DWORD n = 0;
    if (size > 0xFFFFFFFFu) size = 0xFFFFFFFFu;
    if (!WriteFile(reinterpret_cast<HANDLE>(handle_), buffer, static_cast<DWORD>(size), &n, &o)) return -1;
    return n;
}

i64 File::size() const {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(reinterpret_cast<HANDLE>(handle_), &s)) return -1;
    return s.QuadPart;
}

bool File::resize(u64 size) const {
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    return SetFileInformationByHandle(reinterpret_cast<HANDLE>(handle_), FileEndOfFileInfo, &info, sizeof info) != 0;
}

bool File::flush() const { return FlushFileBuffers(reinterpret_cast<HANDLE>(handle_)) != 0; }

FileError make_directory(nx::string_view path) {
    return CreateDirectoryW(wide(path).c_str(), nullptr) ? FileError::None : from_win32(GetLastError());
}

FileError remove_directory(nx::string_view path) {
    return RemoveDirectoryW(wide(path).c_str()) ? FileError::None : from_win32(GetLastError());
}

FileError remove_file(nx::string_view path) {
    const std::wstring w = wide(path);
    if (DeleteFileW(w.c_str())) return FileError::None;
    const DWORD code = GetLastError();
    const DWORD attributes = GetFileAttributesW(w.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) return FileError::IsDirectory;
    return from_win32(code);
}

FileError rename(nx::string_view from, nx::string_view to) {
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING) ? FileError::None
                                                                                        : from_win32(GetLastError());
}

}  // namespace psvrecore::runtime::host

#pragma once

// What the runtime needs from the host OS beyond nx-foundation: giving memory back inside a reservation, reporting
// faults in guest memory, and files open for random-access reading and writing. One implementation per OS family
// (host_win32.cpp, host_posix.cpp).

#include "psvrecore/error.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::runtime::host {

// Returns committed pages to the OS; the range stays reserved and inaccessible.
bool decommit(void* address, usize size);

// A fault in the region [base, base + size): `on_fault` is called with the offset of the address touched and
// whether it was a write, from the faulting thread, before the process ends. Faults elsewhere are left to the OS
// (and nx-foundation's crash reporter). One region at a time; pass nullptr to remove it.
using FaultHandler = void (*)(u64 offset, bool write);
void set_fault_region(void* base, u64 size, FaultHandler on_fault);

enum class OpenMode : u32 {
    Read = 1,
    Write = 2,
    ReadWrite = 3,
};

struct OpenOptions {
    OpenMode mode = OpenMode::Read;
    bool create = false;     // create the file if it does not exist
    bool exclusive = false;  // with create: fail if it exists
    bool truncate = false;
    bool append = false;     // every write goes to the end
};

// Why a file operation failed, in the categories guest error codes need.
enum class FileError {
    None,
    NotFound,
    Exists,
    Access,
    IsDirectory,
    NotDirectory,
    NotEmpty,
    NoSpace,
    Invalid,
    Other,
};

// A file open for reading and/or writing at any position. Thread-safe for positional reads and writes.
class File {
public:
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalid; }
    File& operator=(File&& other) noexcept;
    ~File() { close(); }

    static File open(nx::string_view path, const OpenOptions& options, FileError* error);

    bool valid() const { return handle_ != kInvalid; }
    void close();
    // Bytes transferred, or -1. A read at or past the end transfers 0.
    i64 read_at(u64 offset, void* buffer, u64 size) const;
    i64 write_at(u64 offset, const void* buffer, u64 size) const;
    i64 append(const void* buffer, u64 size) const;  // (for files opened to append)
    i64 size() const;
    bool resize(u64 size) const;
    bool flush() const;

private:
    static constexpr intptr_t kInvalid = -1;
    intptr_t handle_ = kInvalid;
};

// Directories and files by name.
FileError make_directory(nx::string_view path);  // one level
FileError remove_directory(nx::string_view path);  // must be empty
FileError remove_file(nx::string_view path);
FileError rename(nx::string_view from, nx::string_view to);

}  // namespace psvrecore::runtime::host

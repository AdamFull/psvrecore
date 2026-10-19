// Files: SceIofilemgr and the file functions of SceLibKernel.

#include "hle_common.hpp"

#include <cstring>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceLibKernel = 0xCAE9ACE6, kSceIofilemgr = 0xF2FF276E;

FileSystem& files() { return runtime().files(); }

i32 sceIoOpen(Ptr<const char> path, i32 flags, i32 mode) {
    const nx::string p = str(path, 1024);
    i32 r;
    {
        Kernel::Blocking blocking(kernel());
        r = files().open(p, static_cast<u32>(flags), mode);
    }
    nx::logd(logs::io, "sceIoOpen('{}', 0x{:x}) = 0x{:08x}", p, static_cast<u32>(flags), static_cast<u32>(r));
    return r;
}
i32 sceIoClose(i32 fd) {
    const i32 r = files().close(fd);
    nx::logd(logs::io, "sceIoClose(0x{:x}) = 0x{:08x}", fd, static_cast<u32>(r));
    return r;
}

i32 sceIoRead(i32 fd, Ptr<u8> buffer, u32 size) {
    if (size && !writable(buffer.address, size)) return err::kFault;
    i32 r;
    {
        Kernel::Blocking blocking(kernel());
        r = static_cast<i32>(files().read(fd, buffer.get(), size));
    }
    nx::logd(logs::io, "sceIoRead(0x{:x}, 0x{:x} bytes) = 0x{:x}", fd, size, static_cast<u32>(r));
    return r;
}
i32 sceIoWrite(i32 fd, Ptr<const u8> buffer, u32 size) {
    if (size && !memory().valid(buffer.address, size)) return err::kFault;
    i32 r;
    {
        Kernel::Blocking blocking(kernel());
        r = static_cast<i32>(files().write(fd, buffer.get(), size));
    }
    nx::logd(logs::io, "sceIoWrite(0x{:x}, 0x{:x} bytes) = 0x{:x}", fd, size, static_cast<u32>(r));
    return r;
}
i32 sceIoPread(i32 fd, Ptr<u8> buffer, u32 size, i64 offset) {
    if (size && !writable(buffer.address, size)) return err::kFault;
    Kernel::Blocking blocking(kernel());
    return static_cast<i32>(files().pread(fd, buffer.get(), size, offset));
}
i32 sceIoPwrite(i32 fd, Ptr<const u8> buffer, u32 size, i64 offset) {
    if (size && !memory().valid(buffer.address, size)) return err::kFault;
    Kernel::Blocking blocking(kernel());
    return static_cast<i32>(files().pwrite(fd, buffer.get(), size, offset));
}
i64 sceIoLseek(i32 fd, i64 offset, i32 whence) { return files().seek(fd, offset, whence); }
i32 sceIoLseek32(i32 fd, i32 offset, i32 whence) {
    const i64 r = files().seek(fd, offset, whence);
    if (r > 0x7FFFFFFF) return err::kFileTooBig;
    return static_cast<i32>(r);
}

i32 sceIoRemove(Ptr<const char> path) { return files().remove(str(path, 1024)); }
i32 sceIoMkdir(Ptr<const char> path, i32 mode) { return files().mkdir(str(path, 1024), mode); }
i32 sceIoRmdir(Ptr<const char> path) { return files().rmdir(str(path, 1024)); }
i32 sceIoRename(Ptr<const char> from, Ptr<const char> to) { return files().rename(str(from, 1024), str(to, 1024)); }

i32 sceIoDopen(Ptr<const char> path) { return files().dopen(str(path, 1024)); }
i32 sceIoDread(i32 fd, Ptr<io::Dirent> out) {
    if (!writable(out.address, sizeof(io::Dirent))) return err::kFault;
    io::Dirent entry;
    const i32 r = files().dread(fd, &entry);
    if (r > 0) {
        // (d_private is the guest's: a pointer it may have set to receive more; left as it was)
        const u32 keep = out->reserved_pointer;
        std::memcpy(out.get(), &entry, sizeof entry);
        out->reserved_pointer = keep;
    }
    return r;
}
i32 sceIoDclose(i32 fd) { return files().dclose(fd); }

i32 sceIoGetstat(Ptr<const char> path, Ptr<io::Stat> out) {
    if (!writable(out.address, sizeof(io::Stat))) return err::kFault;
    return files().stat(str(path, 1024), out.get());
}
i32 sceIoGetstatByFd(i32 fd, Ptr<io::Stat> out) {
    if (!writable(out.address, sizeof(io::Stat))) return err::kFault;
    return files().stat_fd(fd, out.get());
}
i32 sceIoChstat(Ptr<const char> path, Ptr<const io::Stat>, u32 bits) {
    // Only what the host can keep is kept: nothing (modes and times of guest files are the host's).
    nx::logd(logs::io, "sceIoChstat('{}', 0x{:x}) ignored", str(path, 1024), bits);
    io::Stat st;
    return files().stat(str(path, 1024), &st);
}
i32 sceIoChstatByFd(i32 fd, Ptr<const io::Stat>, u32 bits) {
    nx::logd(logs::io, "sceIoChstatByFd(0x{:x}, 0x{:x}) ignored", static_cast<u32>(fd), bits);
    io::Stat st;
    return files().stat_fd(fd, &st);
}
i32 sceIoSync(Ptr<const char> /*device*/, i32 /*flag*/) { return err::kOk; }
i32 sceIoSyncByFd(i32 fd, i32 /*flag*/) { return files().sync(fd); }

i32 sceIoDevctl(Ptr<const char> device, i32 cmd, Ptr<const void>, u32, Ptr<void>, u32) {
    nx::logw(logs::io, "sceIoDevctl('{}', 0x{:x}) not supported", str(device, 64), static_cast<u32>(cmd));
    return err::kNotSupported;
}
i32 sceIoIoctl(i32 fd, i32 cmd, Ptr<const void>, u32, Ptr<void>, u32) {
    nx::logw(logs::io, "sceIoIoctl(0x{:x}, 0x{:x}) not supported", static_cast<u32>(fd), static_cast<u32>(cmd));
    return err::kNotSupported;
}

const Function kLibKernel[] = {
    HLE_FUNCTION(0x6C60AC61, sceIoOpen),
    HLE_FUNCTION(0x99BA173E, sceIoLseek),
    HLE_FUNCTION(0x52315AD7, sceIoPread),
    HLE_FUNCTION(0x8FFFF5A8, sceIoPwrite),
    HLE_FUNCTION(0xE20ED0F3, sceIoRemove),
    HLE_FUNCTION(0xF737E369, sceIoRename),
    HLE_FUNCTION(0x9670D39F, sceIoMkdir),
    HLE_FUNCTION(0xE9F91EC8, sceIoRmdir),
    HLE_FUNCTION(0xA9283DD0, sceIoDopen),
    HLE_FUNCTION(0x9C8B6624, sceIoDread),
    HLE_FUNCTION(0xBCA5B623, sceIoGetstat),
    HLE_FUNCTION(0x57F8CD25, sceIoGetstatByFd),
    HLE_FUNCTION(0x29482F7F, sceIoChstat),
    HLE_FUNCTION(0x6E903AB2, sceIoChstatByFd),
    HLE_FUNCTION(0x98ACED6D, sceIoSync),
    HLE_FUNCTION(0x04B30CB2, sceIoDevctl),
    HLE_FUNCTION(0x54ABACFA, sceIoIoctl),
};

const Function kIofilemgr[] = {
    HLE_FUNCTION(0xC70B8886, sceIoClose),
    HLE_FUNCTION(0xFDB32293, sceIoRead),
    HLE_FUNCTION(0x34EFD876, sceIoWrite),
    HLE_FUNCTION(0x49252B9B, sceIoLseek32),
    HLE_FUNCTION(0x422A221A, sceIoDclose),
    HLE_FUNCTION(0x16512F59, sceIoSyncByFd),
};

}  // namespace

void register_iofilemgr() {
    register_library({"SceLibKernel", kSceLibKernel, kLibKernel});
    register_library({"SceIofilemgr", kSceIofilemgr, kIofilemgr});
}

}  // namespace psvrecore::runtime::hle

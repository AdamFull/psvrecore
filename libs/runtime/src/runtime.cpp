#include "psvrecore/runtime/runtime.hpp"

#include <atomic>
#include <cstdio>

#include "psvrecore/log.hpp"
#include "psvrecore/recomp/support.hpp"
#include "psvrecore/runtime/hle.hpp"
#include "psvrecore/runtime/host.hpp"

namespace psvrecore::runtime {

namespace {

std::atomic<Runtime*> g_runtime{nullptr};

// A host fault inside the guest address space: say where, and in which guest thread.
void on_guest_fault(u64 offset, bool write) {
    kernel::Thread* t = Kernel::current();
    std::fprintf(stderr, "guest memory fault: %s guest address 0x%08llx", write ? "write to" : "access to",
                 static_cast<unsigned long long>(offset));
    if (offset >= (1ull << 32)) std::fprintf(stderr, " (past the end of the guest address space)");
    if (t) {
        std::fprintf(stderr, " in thread '%s' (UID 0x%08x)", t->name.c_str(), static_cast<u32>(t->uid));
        std::fprintf(stderr, "\n  sp %08x lr %08x r0 %08x r1 %08x r2 %08x r3 %08x", t->ctx.r[13], t->ctx.r[14], t->ctx.r[0],
                     t->ctx.r[1], t->ctx.r[2], t->ctx.r[3]);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

}  // namespace

Runtime* current_runtime() { return g_runtime.load(std::memory_order_acquire); }

Runtime::Runtime(const Config& config) : config_(config) {
    if (current_runtime()) {
        nx::loge(logs::kernel, "a runtime already exists");
        return;
    }
    if (!memory_.init()) {
        nx::loge(logs::kernel, "cannot reserve the guest address space");
        return;
    }
    host::set_fault_region(memory_.base(), (1ull << 32) + (1ull << 20), on_guest_fault);
    Kernel::Config kc = config.kernel;
    kc.stack_region = layout::kStacks;
    kernel_ = nx::make_unique<Kernel>(memory_, kc);
    files_ = nx::make_unique<FileSystem>(config.files);
    loader_ = nx::make_unique<Loader>(*this);
    g_runtime.store(this, std::memory_order_release);
    hle::register_system_libraries();
    ok_ = true;
}

Runtime::~Runtime() {
    if (!ok_) return;
    kernel_->shutdown();
    kernel_.reset();
    loader_.reset();
    files_.reset();
    g_runtime.store(nullptr, std::memory_order_release);
    hle::clear_libraries();
    recomp::clear_modules();
    host::set_fault_region(nullptr, 0, nullptr);
    memory_.shutdown();
}

u32 Runtime::allocate_system(u32 size, nx::string_view name) {
    return memory_.map_anywhere(layout::kSystem, size, GuestMemory::kPageSize, Access::ReadWrite, BlockKind::System, name);
}

void Runtime::exit_process(i32 code) {
    nx::scoped_lock<nx::mutex> guard(exit_lock_);
    if (exited_) return;
    exited_ = true;
    exit_code_ = code;
    exit_changed_.notify_all();
}

bool Runtime::exited() const {
    nx::scoped_lock<nx::mutex> guard(exit_lock_);
    return exited_;
}

i32 Runtime::exit_code() const {
    nx::scoped_lock<nx::mutex> guard(exit_lock_);
    return exit_code_;
}

i32 Runtime::wait_for_exit() {
    nx::scoped_lock<nx::mutex> guard(exit_lock_);
    while (!exited_) exit_changed_.wait(exit_lock_);
    return exit_code_;
}

}  // namespace psvrecore::runtime

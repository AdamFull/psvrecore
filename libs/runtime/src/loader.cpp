#include "psvrecore/runtime/loader.hpp"

#include <cstring>

#include "psvrecore/log.hpp"
#include "psvrecore/formats/module.hpp"
#include "psvrecore/recomp/support.hpp"
#include "psvrecore/runtime/hle.hpp"
#include "psvrecore/runtime/runtime.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::runtime {

namespace {

constexpr u32 kModuleUidBase = 0x40030001u;

// The process parameter block (a version 6 one is 0x34 bytes): the user main thread's parameters, as pointers to
// variables the main module defines (or 0 where it does not).
struct ProcessParam {
    u32 size, magic, version, sdk_version;
    u32 main_thread_name, main_thread_priority, main_thread_stack_size, main_thread_attribute;
    u32 process_name, process_preload_disabled, main_thread_affinity, libc_param, unknown;
};

nx::string hex_fingerprint(const u8* bytes) {
    nx::string s;
    for (int i = 0; i < 8; ++i) s += nx::format("{:02x}", bytes[i]);
    return s;
}

}  // namespace

Loader* current_loader() {
    Runtime* rt = current_runtime();
    return rt ? &rt->loader() : nullptr;
}

Loader::Loader(Runtime& runtime) : runtime_(runtime) {}

void Loader::add_code(const PsvModuleCode& code) {
    nx::scoped_lock<nx::mutex> guard(lock_);
    code_.push_back(&code);
    recomp::register_module(code);
}

Result<i32> Loader::load(nx::string_view host_path, nx::string_view guest_path) {
    // Which generated code this file is, by fingerprint (any layout: the code says where each segment goes).
    auto probe = analysis::Image::load(nx::fs::path_view(host_path));
    if (!probe) return nx::err(probe.error());
    const PsvModuleCode* code = nullptr;
    {
        nx::scoped_lock<nx::mutex> guard(lock_);
        for (const PsvModuleCode* c : code_)
            if (std::memcmp(c->fingerprint, probe->fingerprint().data(), 32) == 0) code = c;
    }
    if (!code)
        return failure(nx::format("{}: no recompiled code for this module (fingerprint {}...): it is not the file the "
                                  "program was generated from",
                                  guest_path, hex_fingerprint(probe->fingerprint().data())));
    if (probe->segments().size() != code->segment_count)
        return failure(nx::format("{}: {} segments, the generated code has {}", guest_path, probe->segments().size(),
                                  code->segment_count));

    // The module where its code expects it, relocated for that.
    auto image = analysis::Image::load(nx::fs::path_view(host_path), std::span<const u32>(code->segment_bases, code->segment_count));
    if (!image) return nx::err(image.error());
    GuestMemory& memory = runtime_.memory();
    nx::vector<u32> mapped;
    for (const auto& s : image->segments()) {
        const nx::string name = nx::format("{} segment {}", image->module().info.name, s.index);
        if (!memory.map_fixed(s.base, s.size(), Access::ReadWrite, BlockKind::Module, name)) {
            for (u32 b : mapped) memory.unmap(b);
            return failure(nx::format("{}: segment {} (0x{:08x}+0x{:x}) overlaps memory already in use", guest_path, s.index,
                                      s.base, s.size()));
        }
        mapped.push_back(s.base);
        std::memcpy(memory.host(s.base), s.bytes.data(), s.size());
    }
    // Code segments become read-only once linked (their data words that refer to imported variables are patched
    // first, in link_module).

    auto m = nx::make_unique<LoadedModule>();
    m->name = image->module().info.name;
    m->path = nx::string(guest_path);
    m->code = code;
    for (const auto& lib : image->module().exports) {
        if (!(lib.attributes & module::kExportMain)) continue;
        for (const auto& f : lib.functions) {
            if (f.nid == module::kNidModuleStart) m->start = f.address;
            else if (f.nid == module::kNidModuleStop) m->stop = f.address;
            else if (f.nid == module::kNidModuleExit) m->exit = f.address;
        }
        for (const auto& v : lib.variables) {
            if (v.nid == module::kNidModuleProcParam) m->process_param = v.address;
            else if (v.nid == module::kNidModuleInfo) m->module_info = v.address;
        }
    }
    m->image = std::move(*image);
    nx::scoped_lock<nx::mutex> guard(lock_);
    m->uid = static_cast<i32>(kModuleUidBase + 2 * static_cast<u32>(next_uid_++));
    nx::logi(logs::loader, "loaded {} ({}) at 0x{:08x}, UID 0x{:08x}", m->name, guest_path,
             m->image.segments().empty() ? 0u : m->image.segments().front().base, static_cast<u32>(m->uid));
    const i32 uid = m->uid;
    modules_.push_back(std::move(m));
    return uid;
}

void Loader::link() {
    nx::scoped_lock<nx::mutex> guard(lock_);
    for (auto& m : modules_) link_module(*m);
}

void Loader::link_module(LoadedModule& m) {
    // Exports of every loaded module (a module's exports come before the system's libraries: a game's own copy of a
    // library is the one it calls).
    auto exported_function = [&](u32 library_nid, u32 nid) -> u32 {
        for (const auto& other : modules_)
            for (const auto& lib : other->image.module().exports)
                if (lib.nid == library_nid && !(lib.attributes & module::kExportMain))
                    for (const auto& f : lib.functions)
                        if (f.nid == nid) return f.address;
        return 0;
    };
    auto exported_variable = [&](u32 library_nid, u32 nid) -> u32 {
        for (const auto& other : modules_)
            for (const auto& lib : other->image.module().exports)
                if (lib.nid == library_nid && !(lib.attributes & module::kExportMain))
                    for (const auto& v : lib.variables)
                        if (v.nid == nid) return v.address;
        return 0;
    };

    u32 unresolved = 0;
    for (u32 i = 0; i < m.code->import_count; ++i) {
        PsvImport& imp = m.code->imports[i];
        const nx::string_view library = imp.library ? imp.library : "";
        if (const u32 at = exported_function(imp.library_nid, imp.nid)) {
            imp.function = recomp::find_function(at);
            if (!imp.function)
                nx::logw(logs::loader, "{}: import 0x{:08x} of {} is exported at 0x{:08x}, where no recompiled function starts",
                         m.name, imp.nid, library, at);
        } else if (const hle::Function* f = hle::find_function(imp.library_nid, library, imp.nid)) {
            imp.function = f->function;
        } else {
            imp.function = nullptr;
            ++unresolved;
            nx::logd(logs::loader, "{}: import 0x{:08x} of {} (0x{:08x}) is not implemented", m.name, imp.nid, library,
                     imp.library_nid);
        }
    }
    for (u32 i = 0; i < m.code->variable_count; ++i) {
        PsvVariable& var = m.code->variables[i];
        const nx::string_view library = var.library ? var.library : "";
        u32 at = exported_variable(var.library_nid, var.nid);
        if (!at) hle::find_variable(var.library_nid, library, var.nid, &at);
        if (!at)
            nx::logd(logs::loader, "{}: variable 0x{:08x} of {} is not provided (its address is 0)", m.name, var.nid, library);
        var.address = at;
    }
    // Data that holds an imported variable's address (in read-only segments too: those are writable meanwhile).
    GuestMemory& memory = runtime_.memory();
    for (const auto& s : m.image.segments())
        if (!s.writable()) memory.protect(s.base, Access::ReadWrite);
    if (auto uses = m.image.variable_uses()) {
        for (const auto& u : *uses)
            if (u.type == 2 && u.variable < m.code->variable_count) {  // (ABS32; code uses read the generated table)
                const u32 value = m.code->variables[u.variable].address + u.addend;
                std::memcpy(memory.host(u.at), &value, 4);
            }
    }
    for (const auto& s : m.image.segments())
        if (!s.writable()) memory.protect(s.base, Access::Read);
    if (unresolved) nx::logi(logs::loader, "{}: {} imports not implemented (a call to one stops the program)", m.name, unresolved);
}

i32 Loader::start(i32 uid, u32 args, u32 argp) {
    u32 entry = 0;
    nx::string name;
    {
        nx::scoped_lock<nx::mutex> guard(lock_);
        for (auto& m : modules_)
            if (m->uid == uid) {
                entry = m->start;
                name = m->name;
                m->started = true;
            }
    }
    if (!entry) return err::kOk;  // (a module without module_start)
    Kernel& kernel = runtime_.kernel();
    const i32 thread = kernel.create_thread(nx::format("{} module_start", name), entry, kernel::kDefaultPriority,
                                            kernel::kMainStackSize, 0, 0);
    if (thread < 0) return thread;
    if (const i32 r = kernel.start_thread(thread, args, argp); r < 0) return r;
    i32 status = 0;
    const i32 r = kernel.wait_thread_end(thread, &status, kernel::Timeout{}, false);
    kernel.delete_thread(thread);
    if (r < 0) return r;
    nx::logi(logs::loader, "{}: module_start returned {}", name, status);
    return status;
}

Result<i32> Loader::boot(const BootOptions& options) {
    FileSystem& files = runtime_.files();
    auto load_guest = [&](nx::string_view guest_path) -> Result<i32> {
        nx::string host_path;
        if (const i32 r = files.resolve(guest_path, &host_path, nullptr, true); r < 0)
            return failure(nx::format("{}: not found (0x{:08x})", guest_path, static_cast<u32>(r)));
        return load(host_path, guest_path);
    };

    auto main_uid = load_guest(options.main);
    if (!main_uid) return nx::err(main_uid.error());
    const LoadedModule* main = find(*main_uid);
    runtime_.process_param = main->process_param;

    for (const auto& path : options.modules) {
        auto uid = load_guest(path);
        if (!uid) return nx::err(uid.error());
        link();
        const i32 r = start(*uid, 0, 0);
        if (r != 0) nx::logw(logs::loader, "{}: module_start returned {}", path, r);
    }
    link();

    // The main thread's parameters, from the main module's process parameter block.
    GuestMemory& memory = runtime_.memory();
    nx::string thread_name = "main";
    i32 priority = kernel::kDefaultPriority;
    u32 stack_size = kernel::kMainStackSize;
    i32 affinity = 0;
    if (runtime_.process_param && memory.valid(runtime_.process_param, sizeof(ProcessParam))) {
        ProcessParam p;
        std::memcpy(&p, memory.host(runtime_.process_param), sizeof p);
        if (p.main_thread_name) thread_name = memory.string(p.main_thread_name, 31);
        if (p.main_thread_priority && memory.valid(p.main_thread_priority, 4)) priority = static_cast<i32>(psv_ld32(p.main_thread_priority));
        if (p.main_thread_stack_size && memory.valid(p.main_thread_stack_size, 4)) stack_size = psv_ld32(p.main_thread_stack_size);
        if (p.size >= 0x2C && p.main_thread_affinity && memory.valid(p.main_thread_affinity, 4))
            affinity = static_cast<i32>(psv_ld32(p.main_thread_affinity));
    }
    if (thread_name.empty()) thread_name = "main";

    // Its arguments: the strings, NUL-terminated one after another.
    nx::vector<u8> block;
    for (const auto& a : options.args) {
        block.insert(block.end(), a.begin(), a.end());
        block.push_back(0);
    }
    u32 argp = 0;
    if (!block.empty()) {
        argp = runtime_.allocate_system(static_cast<u32>(block.size()), "main thread arguments");
        std::memcpy(memory.host(argp), block.data(), block.size());
    }
    if (!main->start) return failure(nx::format("{}: no start entry", options.main));
    Kernel& kernel = runtime_.kernel();
    const i32 thread = kernel.create_thread(thread_name, main->start, priority, stack_size, 0, affinity);
    if (thread < 0) return failure(nx::format("cannot create the main thread (0x{:08x})", static_cast<u32>(thread)));
    if (const i32 r = kernel.start_thread(thread, static_cast<u32>(block.size()), argp); r < 0)
        return failure(nx::format("cannot start the main thread (0x{:08x})", static_cast<u32>(r)));
    nx::logi(logs::loader, "main thread '{}' started (priority {}, stack 0x{:x})", thread_name, priority, stack_size);
    return thread;
}

const LoadedModule* Loader::find(i32 uid) const {
    nx::scoped_lock<nx::mutex> guard(lock_);
    for (const auto& m : modules_)
        if (m->uid == uid) return m.get();
    return nullptr;
}

const LoadedModule* Loader::find_by_address(u32 address) const {
    nx::scoped_lock<nx::mutex> guard(lock_);
    for (const auto& m : modules_)
        if (m->image.segment_of(address)) return m.get();
    return nullptr;
}

const LoadedModule* Loader::find_by_path(nx::string_view guest_path) const {
    nx::scoped_lock<nx::mutex> guard(lock_);
    for (const auto& m : modules_)
        if (m->path == guest_path) return m.get();
    return nullptr;
}

}  // namespace psvrecore::runtime

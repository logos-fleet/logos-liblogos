#include "bare_module_abi.h"
#include "protocol_gate.h"

#include <logos_protocol.h>

#include <filesystem>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace LogosCore {

namespace {

#if defined(_WIN32)
void* imageOpen(const std::string& path)
{
    // No LOCAL/GLOBAL distinction to make: a PE image never publishes its
    // exports into a process-wide namespace, so every LoadLibrary is already
    // what RTLD_LOCAL buys elsewhere.
    return reinterpret_cast<void*>(::LoadLibraryA(path.c_str()));
}

void imageClose(void* handle)
{
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
}

void* imageSymbol(void* handle, const char* name)
{
    return reinterpret_cast<void*>(
        ::GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
}

std::string imageError()
{
    const DWORD code = ::GetLastError();
    return "LoadLibrary/GetProcAddress failed with error " + std::to_string(code);
}
#else
void* imageOpen(const std::string& path)
{
    // RTLD_NOW so an undefined `lp_*` is a load failure we can REPORT, with the
    // missing symbol named, instead of a SIGSEGV on the first call into the
    // module. RTLD_LOCAL so this image's exports stay private — see the header.
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void imageClose(void* handle)
{
    ::dlclose(handle);
}

void* imageSymbol(void* handle, const char* name)
{
    return ::dlsym(handle, name);
}

std::string imageError()
{
    const char* e = ::dlerror();
    return e ? std::string(e) : std::string("unknown dynamic-loader error");
}
#endif

// Resolve `name` into `slot`. Returns false only for a REQUIRED symbol that is
// absent; optional symbols report their absence by leaving `slot` null.
template <typename Fn>
bool resolve(void* handle, const char* name, Fn& slot, bool required,
             std::string* error)
{
    slot = reinterpret_cast<Fn>(imageSymbol(handle, name));
    if (slot || !required)
        return true;
    if (error)
        *error = std::string("the module exports no ") + name +
                 " — it is not a Bare module built against logos-protocol's "
                 "module-impl C ABI";
    return false;
}

} // namespace

bool openBareModule(const std::string& path, BareModuleAbi& out, std::string* error)
{
    closeBareModule(out);

    // Ask the filesystem before asking the loader. Both answer a missing file,
    // but only one of them says so in words a reader can act on: dlopen reports
    // it the same way it reports a plugin whose own dependency is missing.
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(path, ec) || ec) {
        if (error)
            *error = "no Bare module image at '" + path + "'";
        return false;
    }

    void* handle = imageOpen(path);
    if (!handle) {
        if (error)
            *error = "failed to open the Bare module at '" + path + "': " + imageError();
        return false;
    }

    BareModuleAbi abi;
    abi.handle = handle;

    const bool required =
           resolve(handle, "logos_module_dispatch",          abi.dispatch,        true, error)
        && resolve(handle, "logos_module_get_methods",       abi.getMethods,      true, error)
        && resolve(handle, "logos_module_string_free",       abi.stringFree,      true, error)
        && resolve(handle, "logos_module_set_context",       abi.setContext,      true, error)
        && resolve(handle, "logos_module_set_emit_callback", abi.setEmitCallback, true, error)
        && resolve(handle, "logos_module_accept_token",      abi.acceptToken,     true, error)
        && resolve(handle, "logos_module_get_protocol_version", abi.protocolVersion, true, error);

    if (!required) {
        imageClose(handle);
        return false;
    }

    resolve(handle, "logos_module_accept_inbound_token",    abi.acceptInboundToken,    false, nullptr);
    resolve(handle, "logos_module_grant_host_services",     abi.grantHostServices,     false, nullptr);
    resolve(handle, "logos_module_set_call_caller",         abi.setCallCaller,         false, nullptr);
    resolve(handle, "logos_module_set_unload_done_callback", abi.setUnloadDoneCallback, false, nullptr);
    resolve(handle, "logos_module_about_to_unload",         abi.aboutToUnload,         false, nullptr);

    out = abi;
    return true;
}

void closeBareModule(BareModuleAbi& abi)
{
    if (abi.handle)
        imageClose(abi.handle);
    abi = BareModuleAbi{};
}

bool bareModuleProtocolCompatible(const std::string& moduleVersion, std::string* reason)
{
    const auto gate = evaluateProtocolGate(moduleVersion, LOGOS_PROTOCOL_VERSION_MAJOR);
    switch (gate.decision) {
    case ProtocolGateDecision::Allow:
        if (reason) reason->clear();
        return true;
    case ProtocolGateDecision::AllowLegacy:
        if (reason)
            *reason = "the module reports no usable logos-protocol version "
                      "(pre-protocol build) — loading permissively";
        return true;
    case ProtocolGateDecision::Refuse:
        if (reason)
            *reason = "the module was built against logos-protocol " + moduleVersion +
                      " (major " + std::to_string(gate.moduleMajor) + "), this host speaks major " +
                      std::to_string(LOGOS_PROTOCOL_VERSION_MAJOR) +
                      " (" + LOGOS_PROTOCOL_VERSION_STRING + ")";
        return false;
    }
    return false;
}

} // namespace LogosCore

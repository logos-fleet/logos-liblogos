#ifndef BARE_MODULE_ABI_H
#define BARE_MODULE_ABI_H

#include <string>

// A resolved Bare module image.
//
// A **Bare module** (ADR 0006) is a module compiled down to its implementation
// plus its language core and nothing else: no Qt, no logos-protocol inside. It
// exports the module-impl C ABI that logos-protocol declares in
// <logos_module_impl.h> and leaves every `lp_*` symbol UNDEFINED, to be
// resolved upward against the host image's logos-protocol at load time.
//
// This header is the host side of that arrangement, and deliberately the
// Qt-free half: it opens the image, resolves the ABI, and says why when it
// cannot. The Qt provider that DRIVES the resolved ABI is bare_module_glue.h.
//
// The split is what makes the interesting failures testable. "The module has no
// logos_module_dispatch" and "the module was built against an incompatible
// protocol major" are answerable with a file path and no event loop.
namespace LogosCore {

using BareModuleEmitCb       = void (*)(const char* eventName, const char* dataJson, void* userData);
using BareModuleUnloadDoneCb = void (*)(void* userData);

struct BareModuleAbi {
    // Platform handle from dlopen()/LoadLibrary(). Owned; closeBareModule frees it.
    void* handle = nullptr;

    // ── required: a module without these is not a module ──────────────────
    char*       (*dispatch)(const char* method, const char* argsJson) = nullptr;
    char*       (*getMethods)() = nullptr;
    void        (*stringFree)(char* s) = nullptr;
    void        (*setContext)(const char* modulePath, const char* instanceId,
                              const char* instancePersistencePath) = nullptr;
    void        (*setEmitCallback)(BareModuleEmitCb cb, void* userData) = nullptr;
    int         (*acceptToken)(const char* moduleName, const char* token) = nullptr;
    const char* (*protocolVersion)() = nullptr;

    // ── conditional on the protocol the module was generated for ──────────
    // Each arrived in a specific minor (see logos_module_impl.h) and a module
    // generated before it simply does not export the symbol. Absence is a fact
    // about the module's vintage, not a defect, so these are resolved
    // best-effort and null-checked at every call site.
    int  (*acceptInboundToken)(const char* caller, const char* token) = nullptr;  // >= 0.8
    int  (*grantHostServices)(const char* servicesJson) = nullptr;                // >= 0.3
    void (*setCallCaller)(const char* callerJson) = nullptr;                      // >= 0.6
    void (*setUnloadDoneCallback)(BareModuleUnloadDoneCb cb, void* userData) = nullptr;  // >= 0.5
    int  (*aboutToUnload)() = nullptr;                                            // >= 0.5

    explicit operator bool() const { return handle != nullptr; }
};

// Open `path` PRIVATELY (RTLD_LOCAL) and resolve the ABI above.
//
// RTLD_LOCAL rather than RTLD_GLOBAL is the property that lets two Bare modules
// exporting the same symbols coexist in one process: neither image's exports
// enter the global namespace, so the second dlopen cannot be short-circuited
// onto the first one's definitions. Undefined `lp_*` references still resolve
// upward against the host, which is what a Bare module is built to expect.
//
// Returns false and fills `error` (when non-null) on any failure — a missing
// file, an image that will not load, or a required symbol that is absent. It
// never throws and never aborts: a module that fails to open is a load error
// the caller reports, not a crash of the host that tried.
bool openBareModule(const std::string& path, BareModuleAbi& out, std::string* error);

// Close a previously opened image and null the whole table. Safe on a
// default-constructed BareModuleAbi.
void closeBareModule(BareModuleAbi& abi);

// True when `moduleVersion` (as returned by logos_module_get_protocol_version)
// shares this host's logos-protocol MAJOR — the same rule the metadata-stamp
// gate in protocol_gate.h applies, evaluated against the runtime handshake a
// Bare module offers instead of Qt plugin metadata. An unparseable or empty
// version is permissive, with `reason` explaining why.
bool bareModuleProtocolCompatible(const std::string& moduleVersion, std::string* reason);

} // namespace LogosCore

#endif // BARE_MODULE_ABI_H

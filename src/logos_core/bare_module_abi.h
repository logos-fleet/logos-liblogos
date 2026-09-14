#ifndef BARE_MODULE_ABI_H
#define BARE_MODULE_ABI_H

#include <cstddef>
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

// Let go of a previously opened image: the whole ABI table is nulled and the
// host stops reaching into the module. Safe on a default-constructed
// BareModuleAbi.
//
// IT DOES NOT UNMAP THE IMAGE, AND THAT IS THE POINT (#96).
//
// A Bare module is a language core behind a C ABI, and that ABI has no word for
// "my runtime still has threads up". chat_module carries a Rust tokio runtime,
// delivery_module a nim scheduler; both start threads when their context lands,
// both keep them past logos_module_about_to_unload, and both answer that call
// with "already quiescent" — which from inside the module is true, because a
// runtime thread is not a dispatch. The container can quiesce every thread the
// HOST made (BareModuleGlue::stopDispatch) and still have no way to learn about
// those, and the manifest does not declare them either.
//
// So dlclose after an unload is a use-after-free of CODE, and what it costs
// depends entirely on the loader:
//
//   * bionic really unmaps. A surviving module thread executes an unmapped page
//     and the process dies with NO tombstone, no `logcat -b crash` entry and no
//     am_kill — which is exactly how #96 was found: unloading chat_module from
//     the Android Shell's Modules tab took the Shell down 0.8s later.
//   * glibc unmaps too, so `logoscore --container inproc` on desktop Linux has
//     always had the same defect; nothing on that platform had unloaded a
//     threaded Bare module yet.
//   * dyld unmaps a bundle and generally does NOT unmap a framework, which is
//     why the identical sequence round-trips on the iPads: the chat core stays
//     mapped there whatever the host asks for, and the leak is invisible.
//
// The right answer is therefore the one InProcContainer::terminate already
// reaches for when its dispatch thread will not stop — keep the image mapped
// for the life of the process, and carry on — applied to every unload rather
// than only to that one. The module is out of the container's map either way,
// so it is really unloaded: unpublished, its glue destroyed, its emit callback
// cleared. What stays is one mapping, and a reload re-uses it rather than
// mapping a second copy.
//
// WHAT IT COSTS, stated rather than implied. An image kept mapped is memory
// this process never gives back — bounded by the number of DISTINCT module
// images the process ever loaded, not by how often they are unloaded. And a
// reload no longer re-runs the image's initialisers, so a module's statics
// survive an unload/reload cycle. That is already what every iOS host does, so
// this makes the platforms agree rather than adding a new behaviour; a module
// whose re-initialisation matters must do it from logos_module_set_context,
// which is delivered afresh on every load.
//
// The one close that DOES unmap is inside openBareModule, for an image that
// turned out not to export the module ABI: nothing ever handed it a context or
// dispatched into it, and refusing to give back a file that was never a module
// would make every mistyped path cost a mapping for the life of the process.
void closeBareModule(BareModuleAbi& abi);

// How many distinct images this process is holding mapped on the rule above.
//
// Exposed because it is otherwise invisible: it is the number a reader wants
// when the process's footprint does not come back down after an unload, and the
// one thing a test can assert about the decision without asking the loader.
std::size_t retainedBareImageCount();

// True when `moduleVersion` (as returned by logos_module_get_protocol_version)
// shares this host's logos-protocol MAJOR — the same rule the metadata-stamp
// gate in protocol_gate.h applies, evaluated against the runtime handshake a
// Bare module offers instead of Qt plugin metadata. An unparseable or empty
// version is permissive, with `reason` explaining why.
bool bareModuleProtocolCompatible(const std::string& moduleVersion, std::string* reason);

} // namespace LogosCore

#endif // BARE_MODULE_ABI_H

// A Bare module that owns a thread the HOST never learns about.
//
// bare_fixture_module.cpp is the well-behaved module: everything it does
// happens on the thread that dispatched into it, so when the container has
// stopped its dispatch thread, nothing of the module is running. That is not
// what a real Bare module looks like. chat_module carries a Rust core with a
// tokio runtime, delivery_module carries a nim core with its own scheduler;
// both start threads when their context lands and both keep them running past
// `logos_module_about_to_unload`, which the ABI lets them answer "already
// quiescent" (0) because from the module's own point of view they are.
//
// That gap is #96: the container quiesced the one thread it knew about, closed
// the image, and on Android — where the loader really unmaps — the module's own
// thread executed an unmapped page and took the process down with no tombstone.
//
// So this fixture is the bug in miniature. It starts a thread on setContext,
// keeps that thread CALLING CODE IN ITS OWN IMAGE long past the unload, and
// reports itself quiescent when asked. It does not need to be told when the
// unload happens; that it is NOT told is the whole point.
//
// It reports its heartbeat by appending to its instance persistence path, which
// is the one thing the host hands a Bare module that outlives the module's own
// memory. A test can count beats before and after an unload and so tell "the
// thread kept running" from "the thread happened to finish first" — without
// which a passing run would prove nothing.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <logos_protocol.h>

#if defined(_WIN32)
#  define BARE_FIXTURE_EXPORT __declspec(dllexport)
#else
#  define BARE_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {
typedef void (*bare_emit_cb)(const char* eventName, const char* dataJson, void* userData);
typedef void (*bare_unload_done_cb)(void* userData);
}

namespace {

// 120 beats 5ms apart — six tenths of a second of a thread that is still inside
// this image, which is two orders of magnitude longer than the unload it is
// meant to outlive.
constexpr int kBeats = 120;
constexpr int kBeatIntervalMs = 5;

std::atomic<bool> g_started{false};

char* dup(const std::string& s)
{
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out) std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

// NOINLINE and called on every beat, so each beat is a CALL into this image's
// text. Inlined, the thread could run its whole loop out of code the compiler
// had already copied into the caller and an unmapped image would go unnoticed
// for as long as the loop lasted — which would make this fixture prove the
// opposite of what it is for.
__attribute__((noinline)) int beatNumber(int n)
{
    return n + 1;
}

void heartbeat(std::string path)
{
    for (int i = 0; i < kBeats; ++i) {
        const int n = beatNumber(i);
        if (FILE* f = std::fopen(path.c_str(), "a")) {
            std::fprintf(f, "%d\n", n);
            std::fclose(f);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kBeatIntervalMs));
    }
}

} // namespace

extern "C" {

BARE_FIXTURE_EXPORT char* logos_module_get_methods(void)
{
    return dup(R"JSON([
      {"name":"beating","signature":"beating()","returnType":"uint","isInvokable":true}
    ])JSON");
}

BARE_FIXTURE_EXPORT char* logos_module_dispatch(const char* method, const char*)
{
    if (!method) return nullptr;
    if (std::strcmp(method, "beating") == 0)
        return dup(g_started.load() ? "1" : "0");
    return nullptr;
}

BARE_FIXTURE_EXPORT void logos_module_set_context(const char*,
                                                  const char*,
                                                  const char* instancePersistencePath)
{
    // THE CONTEXT IS WHEN A REAL ONE STARTS ITS RUNTIME: it is the first moment
    // a module knows where its state lives, so it is where a language core gets
    // brought up. Started once — a second context delivery must not stack a
    // second thread on an image that is already beating.
    if (!instancePersistencePath || !*instancePersistencePath)
        return;
    if (g_started.exchange(true))
        return;
    std::thread(heartbeat, std::string(instancePersistencePath)).detach();
}

BARE_FIXTURE_EXPORT void logos_module_set_emit_callback(bare_emit_cb, void*) {}

BARE_FIXTURE_EXPORT int logos_module_accept_token(const char* moduleName, const char* token)
{
    return (moduleName && token) ? 0 : -1;
}

BARE_FIXTURE_EXPORT int logos_module_accept_inbound_token(const char* caller, const char* token)
{
    return (caller && token) ? 0 : -1;
}

BARE_FIXTURE_EXPORT int logos_module_grant_host_services(const char* servicesJson)
{
    return servicesJson ? 0 : -1;
}

BARE_FIXTURE_EXPORT void logos_module_set_call_caller(const char*) {}

BARE_FIXTURE_EXPORT void logos_module_set_unload_done_callback(bare_unload_done_cb, void*) {}

BARE_FIXTURE_EXPORT int logos_module_about_to_unload(void)
{
    // "ALREADY QUIESCENT", and honestly so by the ABI's own terms: there is no
    // dispatch in flight and nothing for the host to wait for. The thread this
    // module started is not a dispatch and the ABI has no word for it, which is
    // exactly why the host cannot rely on this answer to decide whether the
    // image may be unmapped.
    return 0;
}

BARE_FIXTURE_EXPORT const char* logos_module_get_protocol_version(void)
{
    return LOGOS_PROTOCOL_VERSION_STRING;
}

BARE_FIXTURE_EXPORT void logos_module_string_free(char* s)
{
    std::free(s);
}

} // extern "C"

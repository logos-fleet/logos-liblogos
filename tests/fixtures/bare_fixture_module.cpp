// A Bare module, hand-written.
//
// The real ones are GENERATED — logos-module-builder's `bare` output compiles a
// module's impl plus its language core and emits this C ABI around it — and
// building one here would drag the whole builder toolchain into a unit test.
// What the Native container actually consumes is the ABI, not the generator, so
// this fixture writes it by hand: no Qt, no logos-protocol, no `lp_*`
// references at all, which also makes it the only Bare module in the tree that
// can be dlopen'd by a test binary with no host image behind it.
//
// It answers a contract with one of each return shape the glue must classify at
// runtime (`uint`, `void`, `result`) plus an event, because those are exactly
// the distinctions the GENERATED Qt glue is handed as compile-time literals and
// that BareModuleGlue has to rediscover from logos_module_get_methods().

#include <cstdlib>
#include <cstring>
#include <string>

// Version macros only — this header declares the lp_* C ABI and includes
// nothing; the fixture calls none of it and links nothing.
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

long long g_total = 0;
bare_emit_cb g_emit = nullptr;
void* g_emitUserData = nullptr;
std::string g_context;
std::string g_lastCaller;
std::string g_lastInboundCaller;

char* dup(const std::string& s)
{
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out) std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

// The first JSON number in `argsJson`, which is all this fixture's arguments
// ever are. Deliberately not a JSON parser: the fixture must not link one, and
// the glue's job — which is what these tests are about — is upstream of it.
long long firstInt(const char* argsJson, long long fallback)
{
    if (!argsJson) return fallback;
    const char* p = argsJson;
    while (*p && *p != '-' && (*p < '0' || *p > '9')) ++p;
    if (!*p) return fallback;
    return std::strtoll(p, nullptr, 10);
}

std::string quoteSwapped(const std::string& in)
{
    std::string out = in;
    for (char& c : out)
        if (c == '"') c = '\'';
    return out;
}

long long secondInt(const char* argsJson, long long fallback)
{
    if (!argsJson) return fallback;
    const char* comma = std::strchr(argsJson, ',');
    if (!comma) return fallback;
    return firstInt(comma, fallback);
}

} // namespace

extern "C" {

BARE_FIXTURE_EXPORT char* logos_module_get_methods(void)
{
    return dup(R"JSON([
      {"name":"add","signature":"add(uint,uint)","returnType":"uint","isInvokable":true,
       "parameters":[{"type":"uint","name":"a"},{"type":"uint","name":"b"}]},
      {"name":"bump","signature":"bump(uint)","returnType":"void","isInvokable":true,
       "parameters":[{"type":"uint","name":"by"}]},
      {"name":"total","signature":"total()","returnType":"uint","isInvokable":true},
      {"name":"describe","signature":"describe()","returnType":"result","isInvokable":true},
      {"name":"context","signature":"context()","returnType":"tstr","isInvokable":true},
      {"name":"caller","signature":"caller()","returnType":"tstr","isInvokable":true},
      {"name":"inboundCaller","signature":"inboundCaller()","returnType":"tstr","isInvokable":true},
      {"name":"tick","signature":"tick(uint)","returnType":"void","isInvokable":true},
      {"type":"event","name":"ticked","signature":"ticked(uint)",
       "parameters":[{"type":"uint","name":"value"}]}
    ])JSON");
}

BARE_FIXTURE_EXPORT char* logos_module_dispatch(const char* method, const char* argsJson)
{
    if (!method) return nullptr;
    const std::string m(method);

    if (m == "add")
        return dup(std::to_string(firstInt(argsJson, 0) + secondInt(argsJson, 0)));
    if (m == "bump") {
        g_total += firstInt(argsJson, 0);
        return dup("null");            // a `void` method: the glue answers true regardless
    }
    if (m == "total")
        return dup(std::to_string(g_total));
    if (m == "describe")
        return dup(R"JSON({"success":true,"value":{"name":"bare_fixture"}})JSON");
    if (m == "context")
        return dup("\"" + g_context + "\"");
    if (m == "caller")
        // Quote-swapped, not escaped: the caller document is itself JSON, and
        // embedding it verbatim inside a JSON string would produce a document
        // the glue cannot parse — which would make this method answer "no
        // caller" for a reason that has nothing to do with the caller.
        return dup("\"" + quoteSwapped(g_lastCaller) + "\"");
    if (m == "inboundCaller")
        return dup("\"" + g_lastInboundCaller + "\"");
    if (m == "tick") {
        const long long value = firstInt(argsJson, 0);
        if (g_emit)
            g_emit("ticked", ("[" + std::to_string(value) + "]").c_str(), g_emitUserData);
        return dup("null");
    }
    return nullptr;                    // unknown method: the documented NULL
}

BARE_FIXTURE_EXPORT void logos_module_set_context(const char* modulePath,
                                                  const char* instanceId,
                                                  const char* instancePersistencePath)
{
    g_context = std::string(modulePath ? modulePath : "") + "|" +
                std::string(instanceId ? instanceId : "") + "|" +
                std::string(instancePersistencePath ? instancePersistencePath : "");
}

BARE_FIXTURE_EXPORT void logos_module_set_emit_callback(bare_emit_cb cb, void* userData)
{
    g_emit = cb;
    g_emitUserData = userData;
}

BARE_FIXTURE_EXPORT int logos_module_accept_token(const char* moduleName, const char* token)
{
    return (moduleName && token) ? 0 : -1;
}

BARE_FIXTURE_EXPORT int logos_module_accept_inbound_token(const char* caller, const char* token)
{
    if (!caller || !token) return -1;
    g_lastInboundCaller = caller;
    return 0;
}

BARE_FIXTURE_EXPORT int logos_module_grant_host_services(const char* servicesJson)
{
    return servicesJson ? 0 : -1;
}

BARE_FIXTURE_EXPORT void logos_module_set_call_caller(const char* callerJson)
{
    // A push records, a NULL pops. One level is all this fixture needs; the
    // per-thread stack the ABI describes is the module SDK's concern.
    g_lastCaller = callerJson ? callerJson : g_lastCaller;
}

BARE_FIXTURE_EXPORT void logos_module_set_unload_done_callback(bare_unload_done_cb, void*) {}

BARE_FIXTURE_EXPORT int logos_module_about_to_unload(void)
{
    return 0;   // already quiescent
}

BARE_FIXTURE_EXPORT const char* logos_module_get_protocol_version(void)
{
    // Reported as this build's own protocol, so the fixture never fails the
    // container's runtime major handshake for a reason the test is not about.
    return LOGOS_PROTOCOL_VERSION_STRING;
}

BARE_FIXTURE_EXPORT void logos_module_string_free(char* s)
{
    std::free(s);
}

} // extern "C"

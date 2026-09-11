#ifndef MODULE_SUPERVISOR_H
#define MODULE_SUPERVISOR_H

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace LogosCore {

// WHAT HAPPENS TO A MODULE THAT DIED WITHOUT BEING ASKED TO.
//
// Before this, nothing did: the load path recorded `loaded -> error` on the
// lifecycle feed and the module stayed gone. That is still the DEFAULT, and
// deliberately — a Qt plugin whose subprocess segfaulted took its state with it,
// and bringing it back without saying so would be a lie told to every consumer
// holding a handle on it.
//
// It stopped being the only defensible answer with the `web` variant (slice 26).
// A Wasm host's whole crash story is that the image traps, the Worker dies, the
// page survives to report it, and the module keeps NO state outside the image's
// own linear memory — so loading it again is not a lie, it is the recovery the
// architecture was cut for. The same is true of any module a host knows to be
// stateless.
//
// So the decision is a POLICY the host states, rather than a behaviour liblogos
// picks: a budget of restarts inside a window, spent per module.
//
// WHY A WINDOW AND NOT A COUNT. A module that crashes once a week for a year is
// healthy and is restarted every time; a module that crashes three times in ten
// seconds is not going to work on the fourth attempt, and restarting it forever
// hides it from the operator who has to fix it — while spinning a page, a
// process or a wasm image each time round. The window is what tells those two
// apart, and it is the only thing that can.

struct SupervisionPolicy {
    // How many times a module may be brought back inside `window`. ZERO — the
    // default — means a module that died stays down, which is what every host
    // got before supervision existed.
    int maxRestarts = 0;

    // The span the budget is counted over, ending at the death being judged.
    std::chrono::milliseconds window{60000};

    // How long to wait before loading it again. A module usually dies because
    // of what it was asked to do, and the caller's retry is often already on
    // its way; coming back instantly means meeting it mid-fall.
    std::chrono::milliseconds backoff{500};
};

// WHO RUNS THE RESTART, and after how long.
//
// Injected rather than called directly because the only two callers want
// opposite things. The shipping one posts to the Qt main thread and waits out
// the backoff: a death is announced on a container's own thread (the subprocess
// container's asio thread, a webview backend's socket pump) and loading a
// module from there would take the load path's locks on a thread that must
// never block, and touch QObject trees it does not own. A test wants the
// opposite — the restart on the calling thread, now — because everything it is
// asserting is what happened, not where.
using RestartScheduler =
    std::function<void(std::chrono::milliseconds delay, std::function<void()> restart)>;

// Install one, or pass {} to restore the shipping scheduler.
void setRestartScheduler(RestartScheduler scheduler);
RestartScheduler restartScheduler();

class ModuleSupervisor {
public:
    enum class Decision {
        Restart,           // within budget: load it again after the backoff
        BudgetExhausted,   // a crash loop; leave it down and say so
        Disabled,          // no policy: the pre-supervision behaviour
    };

    struct Verdict {
        Decision decision = Decision::Disabled;
        // Which death this is inside the window, counting from 1. Carried so
        // the log line can say "2 of 3" rather than only "restarting".
        int attempt = 0;
        std::chrono::milliseconds delay{0};
    };

    ModuleSupervisor() = default;
    explicit ModuleSupervisor(SupervisionPolicy policy) : m_policy(policy) {}

    // The process-wide one, which is what the load path consults.
    static ModuleSupervisor& instance();

    void setPolicy(SupervisionPolicy policy);
    SupervisionPolicy policy() const;

    // Judge one death. RECORDS it as well as judging it, so the caller must
    // call this exactly once per unexpected exit.
    Verdict onUnexpectedExit(const std::string& name,
                             std::chrono::steady_clock::time_point now
                                 = std::chrono::steady_clock::now());

    // Drop a module's history. Called when an operator loads or unloads it by
    // hand: they have intervened, and the deaths of the previous run must not
    // spend the new one's budget.
    void forget(const std::string& name);
    void clear();

    // The operator's way in without a C API call, read once at core start from
    // LOGOS_SUPERVISION: `maxRestarts[,windowMs[,backoffMs]]`, e.g. "3" or
    // "3,60000,250". Anything it cannot read leaves supervision OFF rather than
    // guessing — a typo must not silently restart a crashing module forever.
    static SupervisionPolicy policyFromEnvironment(const char* value);

private:
    mutable std::mutex m_mutex;
    SupervisionPolicy m_policy;
    std::unordered_map<std::string, std::vector<std::chrono::steady_clock::time_point>>
        m_deaths;
};

} // namespace LogosCore

#endif // MODULE_SUPERVISOR_H

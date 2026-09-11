#include "module_supervisor.h"

#include <spdlog/spdlog.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace LogosCore {

namespace {

std::mutex& schedulerMutex()
{
    static std::mutex m;
    return m;
}

RestartScheduler& installedScheduler()
{
    static RestartScheduler scheduler;
    return scheduler;
}

// THE SHIPPING SCHEDULER. Two hops, and each is load-bearing.
//
// ONTO THE QT MAIN THREAD, because a death is announced on whichever thread the
// container noticed it on — the subprocess container's asio thread, a webview
// backend's socket pump — and a load from there would take the load path's
// locks on a thread that must never block behind them, and build QObject trees
// belonging to a thread that is about to go away. The Web container already
// marshals its own retirement here for the same reason.
//
// THEN A TIMER, because the backoff has to be waited out somewhere and it must
// not be by sleeping: the thread that would sleep is the one the restart itself
// has to run on.
//
// A process with no QCoreApplication (a pure-lp host, a unit test) has no such
// thread, so the restart runs on a detached thread of its own — the same
// fallback runOnQtMainThread makes, for the same reason: there is no better
// thread to pick.
void scheduleOnQtMainThread(std::chrono::milliseconds delay, std::function<void()> restart)
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) {
        std::thread([delay, restart = std::move(restart)]() {
            std::this_thread::sleep_for(delay);
            restart();
        }).detach();
        return;
    }

    const int ms = static_cast<int>(delay.count());
    QMetaObject::invokeMethod(
        app,
        [ms, restart = std::move(restart)]() {
            // Created HERE, so the timer belongs to the main thread whatever
            // thread posted this.
            QTimer::singleShot(ms, restart);
        },
        Qt::QueuedConnection);
}

} // namespace

void setRestartScheduler(RestartScheduler scheduler)
{
    std::lock_guard<std::mutex> g(schedulerMutex());
    installedScheduler() = std::move(scheduler);
}

RestartScheduler restartScheduler()
{
    std::lock_guard<std::mutex> g(schedulerMutex());
    if (installedScheduler()) return installedScheduler();
    return &scheduleOnQtMainThread;
}

ModuleSupervisor& ModuleSupervisor::instance()
{
    static ModuleSupervisor supervisor;
    return supervisor;
}

void ModuleSupervisor::setPolicy(SupervisionPolicy policy)
{
    std::lock_guard<std::mutex> g(m_mutex);
    m_policy = policy;
    // A policy change starts the budget over. Setting one is an operator
    // action, and the deaths that happened under the old one describe a system
    // that was being run differently.
    m_deaths.clear();
}

SupervisionPolicy ModuleSupervisor::policy() const
{
    std::lock_guard<std::mutex> g(m_mutex);
    return m_policy;
}

ModuleSupervisor::Verdict ModuleSupervisor::onUnexpectedExit(
    const std::string& name, std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> g(m_mutex);

    Verdict verdict;
    verdict.delay = m_policy.backoff;

    if (m_policy.maxRestarts <= 0) {
        verdict.decision = Decision::Disabled;
        return verdict;
    }

    // The history, with everything older than the window dropped: a module that
    // ran longer than the window before dying again is not in a loop.
    std::vector<std::chrono::steady_clock::time_point>& deaths = m_deaths[name];
    const auto cutoff = now - m_policy.window;
    deaths.erase(std::remove_if(deaths.begin(), deaths.end(),
                                [cutoff](auto t) { return t < cutoff; }),
                 deaths.end());
    deaths.push_back(now);

    verdict.attempt = static_cast<int>(deaths.size());
    verdict.decision = verdict.attempt <= m_policy.maxRestarts
                           ? Decision::Restart
                           : Decision::BudgetExhausted;
    return verdict;
}

void ModuleSupervisor::forget(const std::string& name)
{
    std::lock_guard<std::mutex> g(m_mutex);
    m_deaths.erase(name);
}

void ModuleSupervisor::clear()
{
    std::lock_guard<std::mutex> g(m_mutex);
    m_deaths.clear();
}

SupervisionPolicy ModuleSupervisor::policyFromEnvironment(const char* value)
{
    SupervisionPolicy off;   // maxRestarts == 0
    if (!value || !*value) return off;

    // `maxRestarts[,windowMs[,backoffMs]]`. Parsed strictly: a field that is not
    // a whole number, or a trailing field that is not there at all, leaves
    // supervision off. The alternative is inventing a budget for a typo.
    std::vector<long> fields;
    const std::string text(value);
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t comma = text.find(',', at);
        const std::string field =
            text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (field.empty()) return off;
        try {
            std::size_t used = 0;
            const long parsed = std::stol(field, &used);
            if (used != field.size() || parsed < 0) return off;
            fields.push_back(parsed);
        } catch (...) {
            return off;
        }
        if (comma == std::string::npos) break;
        at = comma + 1;
    }

    if (fields.empty() || fields.size() > 3) return off;

    SupervisionPolicy policy;
    policy.maxRestarts = static_cast<int>(fields[0]);
    if (fields.size() > 1) policy.window = std::chrono::milliseconds(fields[1]);
    if (fields.size() > 2) policy.backoff = std::chrono::milliseconds(fields[2]);
    return policy;
}

} // namespace LogosCore

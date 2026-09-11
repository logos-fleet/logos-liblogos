#ifndef BARE_MODULE_GLUE_H
#define BARE_MODULE_GLUE_H

#include "bare_module_abi.h"

#include <logos_provider_object.h>

#include <QJsonArray>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class QObject;
class QThread;

// THE ONE GENERIC HOST GLUE.
//
// A Qt-plugin module reaches its host through glue the SDK GENERATES from that
// module's LIDL contract — one `<name>_cdylib_glue.cpp` per module, compiled
// into that module, with the contract's shape (which methods return `void`,
// which return `result`) baked in as literal QSets.
//
// The Native container cannot use that glue and does not want to. It holds a
// Bare module: a dlopen'd image with no Qt in it and no per-module C++ compiled
// alongside it. So this class is the same glue written ONCE and parameterised
// at RUNTIME instead of at code-generation time:
//
//   * dispatch is by NAME, with JSON in and JSON out — logos_module_dispatch;
//   * the contract is READ from the module rather than compiled in —
//     logos_module_get_methods() answers the same array
//     LogosProviderObject::getMethods() must return, so the return-shape sets
//     the generated glue spells out as literals are derived from it here;
//   * events arrive through the emit callback the ABI installs.
//
// The consequence worth stating: adding a module to the Bundled set is a build
// input, never a code-generation step. Nothing in this file names a module.
//
// THREADING, AND WHY THE DISPATCH LEAVES THE DELIVERING THREAD.
//
// A published in-process module NEVER runs its handler on the thread the call
// arrived on. Each glue owns ONE worker thread with its own Qt event loop;
// callMethod marshals the dispatch onto it and returns the pending-call
// sentinel (logos_async_dispatch.h), and the worker pushes the real result back
// as a `__logos_call_complete__` event over the provider's existing event
// channel. The consumer transports have awaited that sentinel since protocol
// 0.2, so nothing above the container changes.
//
// IT IS NOT AN OPTIMISATION, it is the only arrangement that terminates. In a
// subprocess the delivering thread belongs to the module's own host process, so
// a handler may block it; in the Native container that thread is the HOST's Qt
// main thread — the one thread every in-process module's outbound replies, and
// every other module's inbound calls, are serviced on. A handler that makes an
// outbound call and waits for it (which `concurrency: "multi"` modules do
// routinely, and every await-style helper does) would be waiting on the thread
// that has to deliver the answer. Measured, before this: the ipc group's
// asyncCall* methods each burned their full 10s await and returned a
// default-constructed value, with the inner call logged as dispatched and never
// completed.
//
// ONE worker, not a pool, so logos_module_impl.h's "the host serializes
// dispatch calls (one at a time)" still holds for every module regardless of
// its declared concurrency: queued invocations on one event loop run in FIFO
// order, one at a time. A `multi` module gets correctness here, not parallelism.
//
// THE UNPUBLISHED GLUE STAYS SYNCHRONOUS. Deferral is switched on by the
// container when — and only when — it has published the module, because the
// sentinel is a PROMISE to deliver a completion over the event channel, and a
// glue with no listener attached has no channel to deliver it on. That is also
// what keeps the hermetic no-LogosAPI path (and this suite's glue tests)
// answering real values from callMethod.
//
// The emit callback, which a module may invoke from any of its own threads, is
// forwarded to the provider's listener on whichever thread emitted it — the
// same as the generated `multi` glue, whose completion event is emitted from
// its worker by construction.
//
// THE SENTINEL IS A PROMISE, INCLUDING DURING TEARDOWN. Answering the sentinel
// commits this glue to delivering exactly one completion under that call id,
// and the consumer transport waits for it — for its whole call timeout if it
// never comes. Unload is asynchronous with every other module's traffic, so a
// call lands mid-teardown routinely, and QThread::quit() INTERRUPTS the worker's
// event dispatcher: the dispatches already posted behind the one in flight are
// never delivered. So every queued call is registered before it is posted and
// claimed by exactly one of the worker and stopDispatch — whichever reaches it
// first — and stopDispatch answers whatever the loop never got to, while the
// event channel is still attached. Once stopped the glue is RETIRED and answers
// new calls itself rather than entering an image the host has already told to
// quiesce.
namespace LogosCore {

class BareModuleGlue : public LogosProviderBase {
public:
    // `abi` must outlive the glue; InProcContainer owns both and destroys the
    // glue first.
    BareModuleGlue(std::string moduleName, std::string moduleVersion, const BareModuleAbi& abi);
    ~BareModuleGlue() override;

    // ── LogosProviderObject ───────────────────────────────────────────────
    QVariant callMethod(const QString& methodName, const QVariantList& args) override;
    QJsonArray getMethods() override;
    bool informModuleToken(const QString& moduleName, const QString& token) override;
    void setEventListener(EventCallback callback) override;
    QString providerName() const override { return QString::fromStdString(m_name); }
    QString providerVersion() const override { return QString::fromStdString(m_version); }

    // Stamp the module's identity/context, exactly as the Qt glue's onInit does
    // from the properties the host hangs on the LogosAPI. Called by the
    // container after registration.
    void deliverContext(const QString& modulePath,
                        const QString& instanceId,
                        const QString& instancePersistencePath);

    // Forward the trust-root grant, when the host's policy designates one.
    void grantHostServices(const QString& servicesJson);

    // Run dispatches on this glue's own worker thread and answer the
    // pending-call sentinel instead of the result. Set by the container once
    // the module is published; see the threading note above for why a glue that
    // is NOT published must never defer.
    //
    // Call before the first dispatch: it starts the worker thread.
    void enableDeferredDispatch();

    // Retire the glue and wait up to `graceMs` for the dispatch in flight.
    //
    // Every call queued and not yet run is answered here with the unloading
    // error, and every call arriving after it is answered the same way without
    // entering the image. Both directions matter: the first keeps a promised
    // completion from going missing, the second keeps a call out of a module
    // that aboutToUnload has already asked to stop.
    //
    // FALSE means the worker is still INSIDE the module image, and the caller
    // must treat that as fatal for the module rather than continuing: the glue
    // must not be destroyed (the queued dispatch holds `this`) and the image
    // must not be closed (a thread is executing code in it). Idempotent, and
    // true when deferral was never on.
    //
    // It can genuinely happen: a handler that is waiting on an outbound reply
    // is waiting on the HOST's main thread, which is the thread calling this.
    // aboutToUnload runs first and is the module's chance to stop; this is what
    // happens when it does not take it.
    bool stopDispatch(int graceMs);

    // Ask the module to quiesce. Returns once it is done or `graceMs` elapsed —
    // logos_module_impl.h is explicit that returning 1 buys a grace period, not
    // a veto.
    void aboutToUnload(int graceMs);

protected:
    void onInit(LogosAPI* api) override;

private:
    static void emitTrampoline(const char* eventName, const char* dataJson, void* userData);
    void onModuleEvent(const QString& eventName, const QVariantList& data);

    // The provider's listener, taken as a COPY under m_eventMutex. Copied
    // rather than invoked under the lock: the listener runs transport code and
    // may re-enter this glue, and it is read from the worker thread while
    // registration writes it from another.
    EventCallback eventListener() const;

    // The dispatch itself: marshal args, push the caller, call the module,
    // classify the result by the contract. Runs on the delivering thread when
    // synchronous and on the worker thread when deferred — it touches nothing
    // that is not either const after construction or owned by the module.
    QVariant dispatchOnThisThread(const QString& methodName,
                                  const QVariantList& args,
                                  const std::string& callerJson);

    // Read once from logos_module_get_methods(): the two return shapes the
    // generated glue would have known at compile time.
    void readContract();

    // What a call gets when the module is on its way out: the same failure
    // token a dispatch the image refused produces, except that a `result`
    // method can say why.
    QVariant unloadingAnswer(const QString& methodName) const;

    // Hand one completion to the provider's listener — from the worker for a
    // dispatch that ran, from stopDispatch for one that never will.
    void deliverCompletion(const QString& callId, const QString& methodName,
                           const QVariant& value);

    // Take `callId` out of m_queued, answering true to exactly ONE caller. That
    // is what makes a completion exactly-once when a stop lands while the
    // worker is part-way through the queue.
    bool claimQueuedCall(const QString& callId);

    // Claim everything still queued, in the order it was issued.
    std::vector<std::pair<QString, QString>> takeQueuedCalls();

    std::string m_name;
    std::string m_version;
    const BareModuleAbi& m_abi;

    QJsonArray m_methods;
    QSet<QString> m_voidMethods;
    QSet<QString> m_resultMethods;

    // Guards m_eventCallback, which is written by the provider at registration
    // and read by the worker thread on every completion.
    mutable std::mutex m_eventMutex;
    EventCallback m_eventCallback;

    // Inline -> Deferred -> Retired, one way, and never back. A mutex rather
    // than an atomic because the state, the worker and the queue have to move
    // TOGETHER: a flip to Retired must not interleave with a call being queued,
    // or that call would be posted onto a loop nobody is going to answer for.
    enum class Dispatch { Inline, Deferred, Retired };

    // The worker and its thread exist only between enableDeferredDispatch and
    // stopDispatch.
    mutable std::mutex m_dispatchMutex;
    Dispatch m_dispatch = Dispatch::Inline;
    QThread* m_workerThread = nullptr;
    QObject* m_workerContext = nullptr;
    // (call id, method name) for every deferred call posted and not yet
    // claimed — one entry per call in flight, so a linear scan is the right
    // shape for it.
    std::vector<std::pair<QString, QString>> m_queued;
    std::atomic<unsigned long long> m_callCounter{0};
};

} // namespace LogosCore

#endif // BARE_MODULE_GLUE_H

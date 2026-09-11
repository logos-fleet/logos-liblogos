#ifndef WEB_MODULE_GLUE_H
#define WEB_MODULE_GLUE_H

#include <logos_provider_object.h>

#include <QJsonArray>
#include <QPointer>

class QEventLoop;
#include <QString>

#include <atomic>
#include <QVariant>
#include <QVariantList>

#include <mutex>
#include <string>
#include <vector>

class LogosObject;

namespace LogosCore {

// THE BRIDGE, AND ONLY THE BRIDGE.
//
// A Web module is a page, and a page cannot be registered on the host's
// provider registry — so the container registers this instead: an ordinary
// LogosProviderObject that answers every question by asking the page over the
// web transport. Every existing consumer — core_service, capability_module,
// another module, the CLI — reaches a Web module exactly as it reaches a
// subprocess one, and nothing above the container learns that the module is
// JavaScript.
//
// It is the same shape as BareModuleGlue and for the same reason: the contract
// is READ at runtime (getMethods is a round trip over the channel, because over
// a transport introspection is a call like any other) rather than generated per
// module, so adding a Web module is an install, never a code-generation step.
//
// IT RELAYS, IT NEVER AUTHORIZES (ADR 0005). Inbound calls were already
// authorized by the ModuleProxy wrapping this object, against this module's own
// isolated token store, before dispatch reached here. What crosses the channel
// carries the module's OWN root credential — the token the core minted for it
// at load and delivered with sendToken — so the page's own ModuleProxy
// validates with transport tag "web", exactly as a TCP module's does.
//
// THE LIMIT, stated rather than implied: the relay presents the module's
// credential, not the ORIGINAL CALLER's. LogosProviderObject::callMethod is
// handed a method and arguments and no caller, so there is nothing here to
// forward; a page therefore cannot distinguish two native callers by token.
// It is the same shape of gap InProcContainer records for a Bare module's
// outbound half, and it closes the same way — when the caller document
// (LogosAPI::currentCallerJson) is threaded through this seam.
class WebModuleGlue : public LogosProviderBase {
public:
    // `page` is the handle the container obtained from the web transport for
    // this module's object. The glue OWNS it and releases it on destruction.
    WebModuleGlue(std::string moduleName, std::string moduleVersion, LogosObject* page);
    ~WebModuleGlue() override;

    // ── LogosProviderObject ───────────────────────────────────────────────
    QVariant callMethod(const QString& methodName, const QVariantList& args) override;
    QJsonArray getMethods() override;
    bool informModuleToken(const QString& moduleName, const QString& token) override;
    void setEventListener(EventCallback callback) override;
    QString providerName() const override { return QString::fromStdString(m_name); }
    QString providerVersion() const override { return QString::fromStdString(m_version); }

    // Deliver the module's OWN root credential to the page — what the core
    // minted at load and what the page's ModuleProxy will validate every
    // inbound call against.
    //
    // SEPARATE FROM informModuleToken, which is capability_module's push of a
    // PAIR token ("this caller may reach you, and here is what it will
    // present"). Both are TokenMessages on the wire; they differ in what the
    // page is expected to do with them, and folding them here would file the
    // module's anchor under a caller key.
    //
    // NO AUTH TOKEN ACCOMPANIES IT, and that is not an oversight: this IS the
    // first credential, so there is nothing yet to authenticate it with. What
    // authorizes it is structural — the frame arrived on the channel this
    // container bound to this page, and nothing outside the container can write
    // that channel (ADR 0005).
    bool deliverCredential(const QString& token);

    // Has the page published its module yet? A round trip: it asks for the
    // contract and answers whether anything came back. This is the Web
    // container's load verdict — see WebContainer::awaitLoad.
    bool pageIsServing();

    // How long a relayed call waits for the page. The default LogosObject
    // timeout, named here so the one place it is chosen is greppable.
    static constexpr int kCallTimeoutMs = 30000;

    // TRUE while this glue is inside a synchronous call into the page.
    //
    // Which is to say: a NESTED EVENT LOOP is running, and below it on the
    // stack sit this object, the QtRO node that dispatched the call into it and
    // everything between. The Web container asks before it destroys a module
    // whose page has died, because a page can die in the middle of the very
    // call that is waiting here — a panic inside a `web` variant's Wasm host is
    // exactly that — and freeing those objects from inside that loop is a
    // use-after-free in the host when the wait unwinds.
    //
    // Atomic because it is read from the thread that noticed the death, which
    // is the backend's, not this one's.
    bool isAwaitingPage() const { return m_awaiting.load() > 0; }

    // STOP WAITING: the page is gone and no Result is coming.
    //
    // Called by the container the moment it learns the page died. Without it a
    // call that was in flight sits out its whole deadline — twenty-one seconds
    // of waiting for an answer from a process that has already exited — and,
    // worse, the module cannot be destroyed for that long, because the wait
    // above is what makes destroying it unsafe. A module whose page dies would
    // then be un-reloadable for the same twenty-one seconds: the old node still
    // holds the name a new one would need.
    void abandonPendingCalls();

private:
    // Call the page and wait for its answer, pumping this thread's event loop
    // when it is the Qt main thread — see the definition, where the reason is a
    // deadlock rather than a preference.
    QVariant awaitPage(const QString& methodName, const QVariantList& args);

    std::atomic<int> m_awaiting{0};

    // The nested loops awaitPage is sitting in, innermost last. QPointer
    // because a loop can also end on its own (its deadline) and the container
    // may be reading this list from another thread while it does.
    mutable std::mutex m_waitMutex;
    std::vector<QPointer<QEventLoop>> m_waits;

    // The credential this relay presents to the page: the module's own root
    // token, as the core minted it and deliverCredential handed it over. Held
    // rather than read from the process ring because it is a property of THIS
    // page's conversation — with two web modules loaded the ambient ring holds
    // one entry per module and the relay must present its own.
    QString authToken() const;

    // The provider's listener, taken as a COPY under m_eventMutex. Copied
    // rather than invoked under the lock: the listener runs transport code and
    // may re-enter this glue, and it is read from the transport's delivery
    // thread while registration writes it from another. Same accessor, for the
    // same reason, as BareModuleGlue::eventListener().
    EventCallback eventListener() const;

    std::string m_name;
    std::string m_version;
    LogosObject* m_page = nullptr;

    // Set by deliverCredential(). Empty until the core mints and sends one,
    // which is after launch() returns — so it cannot be a constructor argument.
    mutable std::mutex m_credentialMutex;
    QString m_credential;

    // Guards m_eventCallback, which the transport's delivery thread reads while
    // the provider's thread may be installing it.
    mutable std::mutex m_eventMutex;
    EventCallback m_eventCallback;
    bool m_subscribed = false;
};

} // namespace LogosCore

#endif // WEB_MODULE_GLUE_H

#ifndef BARE_MODULE_GLUE_H
#define BARE_MODULE_GLUE_H

#include "bare_module_abi.h"

#include <logos_provider_object.h>

#include <QJsonArray>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVariantList>

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
// THREADING. The host serialises dispatch, as logos_module_impl.h requires: a
// glue instance is driven by its ModuleProxy on the thread the transport
// delivers on, and the emit callback (which a module may invoke from any of its
// own threads) is marshalled back onto this object's thread before it reaches
// the provider's event listener.
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

    // Ask the module to quiesce. Returns once it is done or `graceMs` elapsed —
    // logos_module_impl.h is explicit that returning 1 buys a grace period, not
    // a veto.
    void aboutToUnload(int graceMs);

protected:
    void onInit(LogosAPI* api) override;

private:
    static void emitTrampoline(const char* eventName, const char* dataJson, void* userData);
    void onModuleEvent(const QString& eventName, const QVariantList& data);

    // Read once from logos_module_get_methods(): the two return shapes the
    // generated glue would have known at compile time.
    void readContract();

    std::string m_name;
    std::string m_version;
    const BareModuleAbi& m_abi;

    QJsonArray m_methods;
    QSet<QString> m_voidMethods;
    QSet<QString> m_resultMethods;

    EventCallback m_eventCallback;
};

} // namespace LogosCore

#endif // BARE_MODULE_GLUE_H

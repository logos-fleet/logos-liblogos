#include "web_module_view.h"

#include <mutex>

namespace LogosCore {

namespace {

std::mutex& factoryMutex()
{
    static std::mutex mu;
    return mu;
}

WebModuleViewFactory& factoryStorage()
{
    static WebModuleViewFactory factory;
    return factory;
}

} // namespace

void setWebModuleViewFactory(WebModuleViewFactory factory)
{
    std::lock_guard<std::mutex> g(factoryMutex());
    factoryStorage() = std::move(factory);
}

WebModuleViewFactory webModuleViewFactory()
{
    // Copied out under the lock and invoked by the CALLER with it released: a
    // backend that opens a webview may take locks of its own, and holding a
    // process-global mutex across that is how a startup deadlock gets built.
    // Same rule, for the same reason, as logos::web::makeMessageChannel().
    std::lock_guard<std::mutex> g(factoryMutex());
    return factoryStorage();
}

} // namespace LogosCore

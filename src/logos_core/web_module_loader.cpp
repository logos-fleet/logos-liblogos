#include "web_module_loader.h"

namespace LogosCore {

bool WebModuleFormatLoader::canHandle(const ModuleDescriptor& desc) const
{
    // Explicitly, and never by falling through. The Qt-plugin loader claims
    // both "qt-plugin" and the EMPTY format, so a loader that also claimed the
    // empty one would make the answer depend on registration order.
    return desc.format == "web";
}

std::string WebModuleFormatLoader::resolveHostBinary(const ModuleDescriptor& desc) const
{
    return desc.path;
}

std::vector<std::string> WebModuleFormatLoader::buildArguments(const ModuleDescriptor& /*desc*/) const
{
    return {};
}

} // namespace LogosCore

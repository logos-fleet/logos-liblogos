#include "bare_module_loader.h"

namespace LogosCore {

bool BareModuleFormatLoader::canHandle(const ModuleDescriptor& desc) const
{
    // Explicitly, and never by falling through. The Qt-plugin loader claims
    // both "qt-plugin" and the EMPTY format, so a loader that also claimed the
    // empty one would make the answer depend on registration order.
    return desc.format == "bare";
}

std::string BareModuleFormatLoader::resolveHostBinary(const ModuleDescriptor& desc) const
{
    return desc.path;
}

std::vector<std::string> BareModuleFormatLoader::buildArguments(const ModuleDescriptor& /*desc*/) const
{
    return {};
}

} // namespace LogosCore

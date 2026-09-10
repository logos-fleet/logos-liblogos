// Registering a Bare module that is EMBEDDED in the host's own bundle.
//
// Every other way into the registry starts from a package on disk: a directory
// holding manifest.json beside the image, produced by lgpm. On a phone that
// layout is not available where it matters, and the reason is the same on both
// platforms and is not a packaging preference:
//
//   iOS      the only place an app may carry a dylib is <App>.app/Frameworks/,
//            and the image must be signed with the app's identity. A copy made
//            at runtime into the sandbox is unsigned, and dyld refuses it.
//   Android  since API 29 dlopen() of anything under the app's writable data
//            directory is a W^X violation. The image has to stay in the app's
//            native library directory, which is flat and read-only.
//
// Both are read-only directories with no room for a per-module package tree.
// So the manifest travels with the APP rather than beside the image, and this
// is the call that hands the two halves to the registry.
//
// TEST_BARE_MODULE is the suite's own Bare fixture; any real file would do —
// registration does not load the image, it only refuses to name one that is
// not there.

#include <gtest/gtest.h>

#include "module_registry.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>

namespace {

std::string fixtureImage()
{
    const char* p = std::getenv("TEST_BARE_MODULE");
    return p ? p : std::string();
}

std::string manifestFor(const std::string& name, const std::string& version = "1.0.0")
{
    return nlohmann::json{
        {"name", name},
        {"version", version},
        {"type", "core"},
        {"description", "an embedded Bare module"},
        {"dependencies", nlohmann::json::array()},
    }.dump();
}

} // namespace

TEST(EmbeddedBareModuleTest, RegistersUnderTheManifestName)
{
    ASSERT_FALSE(fixtureImage().empty()) << "TEST_BARE_MODULE is not set";

    ModuleRegistry registry;
    const std::string name =
        registry.addEmbeddedBareModule(manifestFor("embedded_counter"), fixtureImage());

    EXPECT_EQ(name, "embedded_counter");
    EXPECT_TRUE(registry.isKnown("embedded_counter"));
    EXPECT_EQ(registry.modulePath("embedded_counter"), fixtureImage());

    const nlohmann::json info = registry.allModulesInfo();
    ASSERT_EQ(info.size(), 1u);
    // The shape decides the container, and nothing about an embedded image
    // says "bare" the way a `_bare` filename does — an iOS framework binary is
    // named after its bundle. So this call asserts it.
    EXPECT_EQ(info[0]["format"], "bare");
    EXPECT_EQ(info[0]["metadata"]["version"], "1.0.0");
}

TEST(EmbeddedBareModuleTest, AnImageThatIsNotThereIsRefused)
{
    ModuleRegistry registry;
    EXPECT_EQ(registry.addEmbeddedBareModule(manifestFor("ghost"), "/nonexistent/libghost_bare.so"),
              std::string());
    EXPECT_FALSE(registry.isKnown("ghost"));
}

TEST(EmbeddedBareModuleTest, AnInvalidModuleNameIsRefused)
{
    ASSERT_FALSE(fixtureImage().empty());
    ModuleRegistry registry;
    // The same trust boundary every other entry point enforces: the name
    // becomes a map key, an RPC target and a directory segment.
    EXPECT_EQ(registry.addEmbeddedBareModule(manifestFor("x/../../victim"), fixtureImage()),
              std::string());
    EXPECT_TRUE(registry.allModulesInfo().empty());
}

TEST(EmbeddedBareModuleTest, MalformedManifestIsRefused)
{
    ASSERT_FALSE(fixtureImage().empty());
    ModuleRegistry registry;
    EXPECT_EQ(registry.addEmbeddedBareModule("{not json", fixtureImage()), std::string());
    EXPECT_EQ(registry.addEmbeddedBareModule(R"({"version":"1.0.0"})", fixtureImage()),
              std::string());
    EXPECT_TRUE(registry.allModulesInfo().empty());
}

TEST(EmbeddedBareModuleTest, ADiscoveryScanDoesNotPruneIt)
{
    ASSERT_FALSE(fixtureImage().empty());

    ModuleRegistry registry;
    ASSERT_EQ(registry.addEmbeddedBareModule(manifestFor("embedded_counter"), fixtureImage()),
              "embedded_counter");

    // discoverInstalledModules() prunes every unloaded module the scan did not
    // see, and an embedded module is in no modules directory BY CONSTRUCTION —
    // so without an exemption the first refresh after registration would erase
    // it. The module is deliberately left UNLOADED here: a loaded one survives
    // pruning for an unrelated reason and would hide the bug.
    registry.discoverInstalledModules();

    EXPECT_TRUE(registry.isKnown("embedded_counter"))
        << "an embedded Bare module must survive a discovery scan that cannot see it";
}

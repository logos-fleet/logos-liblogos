#ifndef DEPENDENCY_RESOLVER_H
#define DEPENDENCY_RESOLVER_H

#include <string>
#include <vector>
#include <functional>

namespace DependencyResolver {

    using IsKnownFn = std::function<bool(const std::string&)>;
    using GetDependenciesFn = std::function<std::vector<std::string>(const std::string&)>;

    // Result of dependency resolution. `order` is a topological sort of
    // the reachable, known modules. `missing` lists dependency names
    // that were referenced but not known to the registry. `hasCycle` is
    // true when the reachable graph contains a cycle (Kahn's algorithm
    // could not consume all nodes). Callers decide policy: load paths
    // treat !ok() as a hard failure; teardown paths may ignore it.
    struct ResolveResult {
        std::vector<std::string> order;
        std::vector<std::string> missing;
        bool hasCycle = false;

        bool ok() const { return missing.empty() && !hasCycle; }
    };

    // `getOptionalDependencies` (metadata.json#optional_dependencies) supplies
    // SOFT edges. They differ from the hard ones in every way that matters here:
    //
    //   - they do NOT expand the set. Requesting a module never pulls its
    //     optional dependencies in, so an absent one is not `missing`.
    //   - they only ORDER modules already in the set, so an optional dependency
    //     requested alongside its dependent comes up first and the dependent's
    //     startup calls land.
    //   - they never make `hasCycle` true. Optional dependencies are what an
    //     author reaches for to BREAK a dependency cycle; reporting one as a
    //     cycle would refuse the configuration the feature exists to allow.
    //     A soft edge that would close a cycle is dropped instead.
    //
    // Omit it (the default) and resolution is exactly what it was.
    ResolveResult resolve(const std::vector<std::string>& requested,
                          IsKnownFn isKnown,
                          GetDependenciesFn getDependencies,
                          GetDependenciesFn getOptionalDependencies = nullptr);
}

#endif // DEPENDENCY_RESOLVER_H

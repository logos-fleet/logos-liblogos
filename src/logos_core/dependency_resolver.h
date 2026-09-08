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
    // An optional dependency best-effort loading left out. Reported rather than
    // logged because "installed, and deliberately not loaded" is invisible
    // otherwise: the module keeps whatever state it had, so nothing on the
    // lifecycle feed marks it, and a caller comparing what it asked for against
    // what came up has no way to tell this from an oversight.
    struct SkippedOptional {
        std::string module;   // the optional dependency that was left out
        std::string namedBy;  // the module whose metadata names it
        // "not_installed" — the optional dependency itself is not known here.
        // "unsatisfiable"  — it is installed, but something it REQUIRES is not;
        //                    `detail` carries the first such module.
        std::string reason;
        std::string detail;
    };

    struct ResolveResult {
        std::vector<std::string> order;
        std::vector<std::string> missing;
        bool hasCycle = false;

        // Names in `order` that are there ONLY because a best-effort optional
        // edge pulled them in. A caller must tolerate their load failing;
        // everything else in `order` is required by something.
        //
        // Empty unless OptionalLoad::BestEffort was asked for. A module that is
        // ALSO reachable by a required edge is not listed: required wins, so a
        // dependency that something genuinely needs never becomes tolerable
        // just because a third module also named it optionally.
        std::vector<std::string> bestEffort;

        // Optional branches best-effort loading declined to take, in the order
        // they were considered. Empty under OptionalLoad::OrderOnly, which
        // declines nothing because it takes nothing.
        std::vector<SkippedOptional> skippedOptional;

        bool ok() const { return missing.empty() && !hasCycle; }

        bool isBestEffort(const std::string& name) const {
            for (const std::string& n : bestEffort)
                if (n == name) return true;
            return false;
        }
    };

    // What an optional dependency does to the CLOSURE. It never changes what a
    // failure means: absent is not an error under either.
    enum class OptionalLoad {
        // Today's behaviour, and the default. Optional edges order modules
        // already in the set and never add one.
        OrderOnly,
        // Additionally pull in every optional dependency that is KNOWN, so a
        // dependent comes up with its optional collaborators when they are
        // installed. An unknown one is skipped silently, and a known one that
        // fails to load is reported through `bestEffort` rather than as a
        // failure of the load.
        BestEffort,
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
                          GetDependenciesFn getOptionalDependencies = nullptr,
                          OptionalLoad optionalLoad = OptionalLoad::OrderOnly);
}

#endif // DEPENDENCY_RESOLVER_H

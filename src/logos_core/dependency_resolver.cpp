#include "dependency_resolver.h"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace DependencyResolver {

    // Kahn over `modulesToLoad` using `edgesOf` as the forward-edge source.
    // Returns the order; short of the full set when the edges cycle.
    static std::vector<std::string> topoSort(
        const std::unordered_set<std::string>& modulesToLoad,
        const std::function<std::vector<std::string>(const std::string&)>& edgesOf)
    {
        std::unordered_map<std::string, std::vector<std::string>> dependents;
        std::unordered_map<std::string, int> inDegree;

        for (const std::string& moduleName : modulesToLoad) {
            if (!inDegree.count(moduleName)) {
                inDegree[moduleName] = 0;
            }

            for (const std::string& depName : edgesOf(moduleName)) {
                if (!depName.empty() && modulesToLoad.count(depName)) {
                    inDegree[moduleName]++;
                    dependents[depName].push_back(moduleName);
                }
            }
        }

        std::deque<std::string> zeroInDegree;

        for (const std::string& moduleName : modulesToLoad) {
            if (inDegree.count(moduleName) == 0 || inDegree.at(moduleName) == 0) {
                zeroInDegree.push_back(moduleName);
            }
        }

        std::vector<std::string> order;
        while (!zeroInDegree.empty()) {
            std::string moduleName = zeroInDegree.front();
            zeroInDegree.pop_front();
            order.push_back(moduleName);

            auto it = dependents.find(moduleName);
            if (it != dependents.end()) {
                for (const std::string& dependent : it->second) {
                    inDegree[dependent]--;
                    if (inDegree[dependent] == 0) {
                        zeroInDegree.push_back(dependent);
                    }
                }
            }
        }
        return order;
    }

    ResolveResult resolve(const std::vector<std::string>& requested,
                          IsKnownFn isKnown,
                          GetDependenciesFn getDependencies,
                          GetDependenciesFn getOptionalDependencies,
                          OptionalLoad optionalLoad) {
        ResolveResult out;

        const bool bestEffort =
            optionalLoad == OptionalLoad::BestEffort && getOptionalDependencies;

        std::unordered_set<std::string> modulesToLoad;
        std::deque<std::string> queue(requested.begin(), requested.end());

        while (!queue.empty()) {
            std::string moduleName = queue.front();
            queue.pop_front();

            if (modulesToLoad.count(moduleName))
                continue;

            if (!isKnown(moduleName)) {
                spdlog::warn("Module not found in known modules: {}", moduleName);
                out.missing.push_back(moduleName);
                continue;
            }

            modulesToLoad.insert(moduleName);

            for (const std::string& depName : getDependencies(moduleName)) {
                if (!depName.empty() && !modulesToLoad.count(depName)) {
                    queue.push_back(depName);
                }
            }

        }

        if (!out.missing.empty()) {
            std::string joined;
            for (std::size_t i = 0; i < out.missing.size(); ++i) {
                if (i > 0) joined += ", ";
                joined += out.missing[i];
            }
            spdlog::warn("Missing dependencies detected: {}", joined);
        }

        // Best effort runs as a SECOND pass, over the required closure above.
        //
        // Not inline in that walk, and this is the whole of the difference: a
        // queued name that is not installed becomes `missing`, and `missing` is
        // a hard failure. Expanding optional edges in the same queue therefore
        // makes an optional dependency's own unsatisfiable subtree fail the
        // load of the module that merely NAMED it — which is the property this
        // dependency kind exists to prevent. Measured before it was fixed: with
        // app -opt-> extra -req-> ghost(absent), loading `app` returned 0.
        //
        // So a branch is admitted only if it is WHOLLY satisfiable: every
        // module in the optional dependency's own required closure is
        // installed. If any is not, the branch is dropped entire — loading a
        // module whose dependencies are missing would only fail later, noisily,
        // for something nobody required.
        //
        // Fixed point, so an optional dependency of an optional dependency is
        // reached on a later round.
        if (bestEffort) {
            std::unordered_set<std::string> reportedSkips;
            bool grew = true;
            while (grew) {
                grew = false;
                std::vector<std::string> frontier(modulesToLoad.begin(), modulesToLoad.end());
                for (const std::string& holder : frontier) {
                    for (const std::string& optName : getOptionalDependencies(holder)) {
                        if (optName.empty() || modulesToLoad.count(optName))
                            continue;

                        // The candidate branch: `optName` and everything it
                        // REQUIRES, gathered before anything is committed.
                        std::unordered_set<std::string> branch;
                        std::deque<std::string> probe{optName};
                        std::string blocker;
                        while (!probe.empty() && blocker.empty()) {
                            std::string n = probe.front();
                            probe.pop_front();
                            if (branch.count(n) || modulesToLoad.count(n))
                                continue;
                            if (!isKnown(n)) { blocker = n; break; }
                            branch.insert(n);
                            for (const std::string& d : getDependencies(n))
                                if (!d.empty() && !branch.count(d) && !modulesToLoad.count(d))
                                    probe.push_back(d);
                        }

                        if (!blocker.empty()) {
                            // Reported once per (holder, optional) pair: the
                            // fixed-point loop revisits every holder each round,
                            // and a declined branch stays declined.
                            const std::string key = holder + '\0' + optName;
                            if (reportedSkips.insert(key).second) {
                                out.skippedOptional.push_back(SkippedOptional{
                                    optName, holder,
                                    blocker == optName ? "not_installed" : "unsatisfiable",
                                    blocker == optName ? std::string{} : blocker});
                                spdlog::debug("Optional dependency '{}' of '{}' left out: {} is not installed",
                                              optName, holder, blocker);
                            }
                            continue;
                        }
                        for (const std::string& n : branch)
                            modulesToLoad.insert(n);
                        grew = grew || !branch.empty();
                    }
                }
            }
        }

        // Which of those are tolerable to fail: everything the REQUIRED edges
        // alone could not have reached. Computed by re-walking the hard graph
        // from `requested` and subtracting, rather than by tagging nodes as
        // they are queued, because a module can be reached BOTH ways and the
        // order in which the queue happens to reach it must not decide whether
        // its failure is fatal. Required wins, always.
        if (bestEffort) {
            std::unordered_set<std::string> requiredOnly;
            std::deque<std::string> hardQueue(requested.begin(), requested.end());
            while (!hardQueue.empty()) {
                std::string n = hardQueue.front();
                hardQueue.pop_front();
                if (requiredOnly.count(n) || !isKnown(n))
                    continue;
                requiredOnly.insert(n);
                for (const std::string& depName : getDependencies(n))
                    if (!depName.empty() && !requiredOnly.count(depName))
                        hardQueue.push_back(depName);
            }
            for (const std::string& n : modulesToLoad)
                if (!requiredOnly.count(n))
                    out.bestEffort.push_back(n);
            std::sort(out.bestEffort.begin(), out.bestEffort.end());
        }

        // Hard edges decide BOTH the closure and whether this is a cycle.
        out.order = topoSort(modulesToLoad, getDependencies);

        // Soft edges only refine the order, and only when they can. An
        // optional dependency requested alongside its dependent should come up
        // first, but an optional edge that closes a cycle is dropped rather
        // than reported -- breaking a cycle is what optional dependencies are
        // for. Falling back wholesale (rather than removing one edge) keeps the
        // answer a topological order of the hard graph, which is the only
        // ordering the loader actually requires.
        if (getOptionalDependencies && out.order.size() == modulesToLoad.size()) {
            auto combinedEdges = [&](const std::string& n) {
                std::vector<std::string> edges = getDependencies(n);
                for (const std::string& soft : getOptionalDependencies(n))
                    edges.push_back(soft);
                return edges;
            };
            std::vector<std::string> refined = topoSort(modulesToLoad, combinedEdges);
            if (refined.size() == modulesToLoad.size())
                out.order = std::move(refined);
            else
                spdlog::debug("Optional dependency edges would cycle; "
                              "ordering by required dependencies alone");
        }

        if (out.order.size() < modulesToLoad.size()) {
            out.hasCycle = true;
            std::string cycleJoined;
            bool first = true;
            for (const std::string& moduleName : modulesToLoad) {
                bool inResult = false;
                for (const auto& r : out.order) {
                    if (r == moduleName) { inResult = true; break; }
                }
                if (!inResult) {
                    if (!first) cycleJoined += ", ";
                    cycleJoined += moduleName;
                    first = false;
                }
            }
            spdlog::critical("Circular dependency detected involving modules: {}", cycleJoined);
        }

        return out;
    }

}

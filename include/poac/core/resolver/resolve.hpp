#ifndef POAC_CORE_RESOLVER_RESOLVE_HPP
#define POAC_CORE_RESOLVER_RESOLVE_HPP

// std
#include <iostream>
#include <vector>
#include <stack>
#include <string>
#include <string_view>
#include <sstream>
#include <regex>
#include <utility>
#include <map>
#include <unordered_map>
#include <optional>
#include <algorithm>
#include <iterator>

// external
#include <boost/dynamic_bitset.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <fmt/core.h>
#include <mitama/result/result.hpp>
#include <plog/Log.h>

// internal
#include <poac/config.hpp>
#include <poac/core/except.hpp>
#include <poac/core/name.hpp>
#include <poac/core/resolver/sat.hpp>
#include <poac/io/net.hpp>
#include <poac/io/path.hpp>
#include <poac/util/meta.hpp>
#include <poac/util/semver/semver.hpp>

namespace poac::core::resolver::resolve {
    struct with_deps : std::true_type {};
    struct without_deps : std::false_type {};

    // Duplicate dependencies mean not resolved,
    //   so a package has version not interval generally.
    // A package depends packages have many different versions of one name,
    //   so this should not be std::unordered_map.
    // Also, package information does not need
    //   package's dependency's dependencies,
    //   so second value of std::pair is not Package (this type)
    //   just needs std::string indicated specific version.
    template <class WithDeps>
    struct duplicate_deps {};

    template <class WithDeps>
    using duplicate_deps_t = typename duplicate_deps<WithDeps>::type;

    template <>
    struct duplicate_deps<without_deps> {
        using type = std::vector<std::pair<std::string, std::string>>;
    };

    using package_t = std::pair<
        std::string, // name
        std::string // version or interval
    >;

    package_t::first_type&
    get_name(package_t& package) noexcept {
        return package.first;
    }

    const package_t::first_type&
    get_name(const package_t& package) noexcept {
        return package.first;
    }

    package_t::first_type&
    get_version(package_t& package) noexcept {
        return package.second;
    }

    const package_t::first_type&
    get_version(const package_t& package) noexcept {
        return package.second;
    }

    using deps_t = std::optional<duplicate_deps_t<without_deps>>;

    template <>
    struct duplicate_deps<with_deps> {
        using type = std::vector<std::pair<package_t, deps_t>>;
    };

    struct hash_pair {
        template <class T, class U>
        std::size_t operator()(const std::pair<T, U>& p) const {
            return std::hash<T>()(p.first) ^ std::hash<U>()(p.second);
        }
    };

    template <class WithDeps>
    using unique_deps_t =
        std::conditional_t<
            WithDeps::value,
            std::unordered_map<package_t, deps_t, hash_pair>,
            std::unordered_map<
                std::string, // name
                std::string  // version or interval
            >>;

    std::string to_binary_numbers(const int& x, const std::size_t& digit) {
        return fmt::format(FMT_STRING("{:0{}b}"), x, digit);
    }

    // A ∨ B ∨ C
    // A ∨ ¬B ∨ ¬C
    // ¬A ∨ B ∨ ¬C
    // ¬A ∨ ¬B ∨ C
    // ¬A ∨ ¬B ∨ ¬C
    void multiple_versions_cnf(const std::vector<int>& clause, std::vector<std::vector<int>>& clauses) {
        const int combinations = 1 << clause.size();
        for (int i = 0; i < combinations; ++i) { // index sequence
            boost::dynamic_bitset<> bs(to_binary_numbers(i, clause.size()));
            if (bs.count() == 1) {
                continue;
            }

            std::vector<int> new_clause;
            for (std::size_t j = 0; j < bs.size(); ++j) {
                new_clause.emplace_back(bs[j] ? clause[j] * -1 : clause[j]);
            }
            clauses.emplace_back(new_clause);
        }
    }

    std::vector<std::vector<int>>
    create_cnf(const duplicate_deps_t<with_deps>& activated) {
        std::vector<std::vector<int>> clauses;
        std::vector<int> already_added;

        auto first = std::cbegin(activated);
        auto last = std::cend(activated);
        for (int i = 0; i < static_cast<int>(activated.size()); ++i) {
            if (util::meta::find(already_added, i)) {
                continue;
            }

            const auto name_lambda = [&](const auto& x){
                return x.first == activated[i].first;
            };
            // 現在指すパッケージと同名の他のパッケージは存在しない
            if (const auto count = std::count_if(first, last, name_lambda); count == 1) {
                std::vector<int> clause;
                clause.emplace_back(i + 1);
                clauses.emplace_back(clause);

                // index ⇒ deps
                if (!activated[i].second.has_value()) {
                    clause[0] *= -1;
                    for (const auto& [name, version] : activated[i].second.value()) {
                        // 必ず存在することが保証されている
                        clause.emplace_back(
                            util::meta::index_of_if(
                                first, last,
                                [&n=name, &v=version](const auto& d){
                                    return d.first.first == n &&
                                           d.first.second == v;
                                }) + 1);
                    }
                    clauses.emplace_back(clause);
                }
            }
            else if (count > 1) {
                std::vector<int> clause;

                for (auto found = first; found != last; found = std::find_if(found, last, name_lambda)) {
                    const auto index = std::distance(first, found);
                    clause.emplace_back(index + 1);
                    already_added.emplace_back(index + 1);

                    // index ⇒ deps
                    if (!found->second.has_value()) {
                        std::vector<int> new_clause;
                        new_clause.emplace_back(index);
                        for (const auto& package : found->second.value()) {
                            // 必ず存在することが保証されている
                            new_clause.emplace_back(
                                util::meta::index_of_if(
                                    first, last,
                                    [&package=package](const auto& p){
                                        return p.first.first == package.first &&
                                               p.first.second == package.second;
                                    }
                                ) + 1
                            );
                        }
                        clauses.emplace_back(new_clause);
                    }
                    ++found;
                }
                multiple_versions_cnf(clause, clauses);
            }
        }
        return clauses;
    }

    unique_deps_t<with_deps>
    solve_sat(const duplicate_deps_t<with_deps>& activated, const std::vector<std::vector<int>>& clauses) {
        unique_deps_t<with_deps> resolved_deps{};
        // deps.activated.size() == variables
        const auto [result, assignments] = sat::solve(clauses, activated.size());
        if (result == sat::Sat::completed) {
            PLOG_DEBUG << "SAT";
            for (const auto& a : assignments) {
                PLOG_DEBUG << a << " ";
                if (a > 0) {
                    const auto& [package, deps] = activated[a - 1];
                    resolved_deps.emplace(package, deps);
                }
            }
            PLOG_DEBUG << 0;
        } else {
            throw except::error("Could not solve in this dependencies.");
        }
        return resolved_deps;
    }

    unique_deps_t<with_deps>
    backtrack_loop(const duplicate_deps_t<with_deps>& activated) {
        const auto clauses = create_cnf(activated);
        IF_PLOG(plog::debug) {
            for (const auto& c : clauses) {
                for (const auto& l : c) {
                    const auto& [package, deps] =
                        activated[l > 0 ? l - 1 : (l * -1) - 1];
                    PLOG_DEBUG <<
                        fmt::format(
                            "{}-{}: {}, ",
                            package.first, package.second, l
                        );
                }
                PLOG_DEBUG << "";
            }
        }
        return solve_sat(activated, clauses);
    }

    unique_deps_t<with_deps>
    activated_to_backtracked(const duplicate_deps_t<with_deps>& activated_deps) {
        unique_deps_t<with_deps> resolved_deps;
        for (const auto& [package, deps] : activated_deps) {
            resolved_deps.emplace(package, deps);
        }
        return resolved_deps;
    }

    template <class SinglePassRange>
    bool duplicate_loose(const SinglePassRange& rng) {
        const auto first = std::begin(rng);
        const auto last = std::end(rng);
        return std::find_if(first, last, [&](const auto& x){
            return std::count_if(first, last, [&](const auto& y) {
              return x.first == y.first;
            }) > 1;
        }) != last;
    }

    // Interval to multiple versions
    // `>=0.1.2 and <3.4.0` -> { 2.4.0, 2.5.0 }
    // `latest` -> { 2.5.0 }
    // name is boost/config, no boost-config
    std::vector<std::string>
    get_versions_satisfy_interval(const std::string& name, const std::string& interval) {
        // TODO: (`>1.2 and <=1.3.2` -> NG，`>1.2.0-alpha and <=1.3.2` -> OK)
        const std::vector<std::string> versions =
            io::net::api::versions(name).unwrap();
        if (interval == "latest") {
            return {
                *std::max_element(
                    versions.cbegin(),
                    versions.cend(),
                    [](auto a, auto b){
                        return semver::Version(a) > b;
                    }
                )
            };
        } else { // `2.0.0` specific version or `>=0.1.2 and <3.4.0` version interval
            std::vector<std::string> res;
            semver::Interval i(name, interval);
            std::copy_if(
                versions.cbegin(),
                versions.cend(),
                back_inserter(res),
                [&](std::string s) { return i.satisfies(s); }
            );
            if (res.empty()) {
                throw except::error(
                    fmt::format(
                        "`{}: {}` not found. seem dependencies are broken",
                        name, interval
                    )
                );
            } else {
                return res;
            }
        }
    }

    using interval_cache_t =
        std::vector<
            std::tuple<
                std::string, // name
                std::string, // interval
                std::vector<std::string> // versions in the interval
            >>;

    duplicate_deps_t<without_deps>
    gather_deps_of_deps(
        const unique_deps_t<without_deps>& deps_api_res,
        interval_cache_t& interval_cache)
    {
        duplicate_deps_t<without_deps> cur_deps_deps;
        for (const auto& [name, interval] : deps_api_res) {
            // Check if node package is resolved dependency (by interval)
            const auto itr =
                boost::range::find_if(
                    interval_cache,
                    [&name=name, &interval=interval](const auto& d) {
                      return std::get<0>(d) == name &&
                             std::get<1>(d) == interval;
                    }
                );
            if (itr != interval_cache.cend()) { // cache found
                for (const auto& dep_version : std::get<2>(*itr)) {
                    cur_deps_deps.emplace_back(name, dep_version);
                }
            } else {
                const auto dep_versions =
                    get_versions_satisfy_interval(name, interval);
                // Cache interval and versions pair
                interval_cache.emplace_back(name, interval, dep_versions);
                for (const auto& dep_version : dep_versions) {
                    cur_deps_deps.emplace_back(name, dep_version);
                }
            }
        }
        return cur_deps_deps;
    }

    void gather_deps(
        const std::string& name,
        const std::string& version,
        duplicate_deps_t<with_deps>& new_deps,
        interval_cache_t& interval_cache)
    {
        // Check if root package resolved dependency
        //   (whether the specific version is the same),
        //   and check circulating
        if (util::meta::find_if(new_deps, [&](const auto& d) {
                return d.first.first == name && d.first.second == version;
        })) {
            return;
        }

        // Get dependencies of dependencies
        const unique_deps_t<without_deps> deps_api_res =
            io::net::api::deps(name, version).unwrap();
        if (deps_api_res.empty()) {
            new_deps.emplace_back(std::make_pair(name, version), std::nullopt);
        } else {
            const auto deps_of_deps = gather_deps_of_deps(deps_api_res, interval_cache);

            // Store dependency and the dependency's dependencies.
            new_deps.emplace_back(std::make_pair(name, version), deps_of_deps);

            // Gather dependencies of dependencies of dependencies. lol
            for (const auto& [name, version] : deps_of_deps) {
                gather_deps(name, version, new_deps, interval_cache);
            }
        }
    }

    [[nodiscard]] mitama::result<duplicate_deps_t<with_deps>, std::string>
    gather_all_deps(const unique_deps_t<without_deps>& deps) {
        duplicate_deps_t<with_deps> duplicate_deps{};
        interval_cache_t interval_cache{};

        // Activate the root of dependencies
        for (const auto& package : deps) {
            // Check whether the packages specified in poac.toml
            //   are already resolved which includes
            //   that package's dependencies and package's versions
            //   by checking whether package's interval is the same.
            if (util::meta::find_if(
                    interval_cache,
                    [&package=package](const auto& cache){
                        return get_name(package) == std::get<0>(cache) &&
                              get_version(package) == std::get<1>(cache);
                    }
                )) {
                continue;
            }

            // Get versions using interval
            // FIXME: versions API and deps API are received the almost same responses
            const auto versions =
                get_versions_satisfy_interval(
                    get_name(package),
                    get_version(package)
                );
            // Cache interval and versions pair
            interval_cache.emplace_back(
                get_name(package),
                get_version(package),
                versions
            );
            for (const auto& version : versions) {
                gather_deps(
                    get_name(package),
                    version,
                    duplicate_deps,
                    interval_cache
                );
            }
        }
        return mitama::success(duplicate_deps);
    }
} // end namespace
#endif // !POAC_CORE_RESOLVER_RESOLVE_HPP

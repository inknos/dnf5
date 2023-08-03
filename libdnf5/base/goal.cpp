/*
Copyright Contributors to the libdnf project.

This file is part of libdnf: https://github.com/rpm-software-management/libdnf/

Libdnf is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 2.1 of the License, or
(at your option) any later version.

Libdnf is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with libdnf.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "libdnf5/base/goal.hpp"

#include "advisory/advisory_package_private.hpp"
#include "base_private.hpp"
#include "module/module_goal_private.hpp"
#include "module/module_sack_impl.hpp"
#include "rpm/package_query_impl.hpp"
#include "rpm/package_sack_impl.hpp"
#include "rpm/package_set_impl.hpp"
#include "rpm/solv/goal_private.hpp"
#include "solv/id_queue.hpp"
#include "solv/pool.hpp"
#include "transaction_impl.hpp"
#include "utils/string.hpp"
#include "utils/url.hpp"

#include "libdnf5/common/exception.hpp"
#include "libdnf5/comps/environment/query.hpp"
#include "libdnf5/comps/group/query.hpp"
#include "libdnf5/module/module_errors.hpp"
#include "libdnf5/rpm/package_query.hpp"
#include "libdnf5/rpm/reldep.hpp"
#include "libdnf5/utils/bgettext/bgettext-mark-domain.h"
#include "libdnf5/utils/patterns.hpp"

#include <filesystem>
#include <iostream>
#include <map>

namespace {

void add_obsoletes_to_data(const libdnf5::rpm::PackageQuery & base_query, libdnf5::rpm::PackageSet & data) {
    libdnf5::rpm::PackageQuery data_query(data);

    // In case there is an installed package in the `data` behave consistently
    // with upgrade and add all the obsoleters.
    libdnf5::rpm::PackageQuery installed_data(data_query);
    installed_data.filter_installed();

    if (installed_data.empty()) {
        // If there is no installed package in the `data`, add only obsoleters
        // of the latest versions.  This should prevent unexpected results in
        // case a package has multiple versions and some older version is being
        // obsoleted.
        // See also https://bugzilla.redhat.com/show_bug.cgi?id=2176263
        data_query.filter_priority();
        data_query.filter_latest_evr();
    }

    libdnf5::rpm::PackageQuery obsoletes_query(base_query);
    obsoletes_query.filter_obsoletes(data_query);
    data |= obsoletes_query;
}


}  // namespace


namespace libdnf5 {

namespace {

inline bool name_arch_compare_lower_solvable(const Solvable * first, const Solvable * second) {
    if (first->name != second->name) {
        return first->name < second->name;
    }
    return first->arch < second->arch;
}


}  // namespace

using GroupSpec = std::tuple<GoalAction, libdnf5::transaction::TransactionItemReason, std::string, GoalJobSettings>;

class Goal::Impl {
public:
    Impl(const BaseWeakPtr & base);
    ~Impl();

    void add_rpm_ids(GoalAction action, const rpm::Package & rpm_package, const GoalJobSettings & settings);
    void add_rpm_ids(GoalAction action, const rpm::PackageSet & package_set, const GoalJobSettings & settings);

    GoalProblem add_specs_to_goal(base::Transaction & transaction);
    GoalProblem resolve_group_specs(std::vector<GroupSpec> & specs, base::Transaction & transaction);
    void add_resolved_group_specs_to_goal(base::Transaction & transaction);
    void add_resolved_environment_specs_to_goal(base::Transaction & transaction);
    GoalProblem add_module_specs_to_goal(base::Transaction & transaction);
    GoalProblem add_reason_change_specs_to_goal(base::Transaction & transaction);

    std::pair<GoalProblem, libdnf5::solv::IdQueue> add_install_to_goal(
        base::Transaction & transaction, GoalAction action, const std::string & spec, GoalJobSettings & settings);
    void add_provide_install_to_goal(const std::string & spec, GoalJobSettings & settings);
    GoalProblem add_reinstall_to_goal(
        base::Transaction & transaction, const std::string & spec, GoalJobSettings & settings);
    void add_remove_to_goal(base::Transaction & transaction, const std::string & spec, GoalJobSettings & settings);
    GoalProblem add_up_down_distrosync_to_goal(
        base::Transaction & transaction,
        GoalAction action,
        const std::string & spec,
        GoalJobSettings & settings,
        bool minimal = false);
    void add_rpms_to_goal(base::Transaction & transaction);

    static void filter_candidates_for_advisory_upgrade(
        const BaseWeakPtr & base,
        libdnf5::rpm::PackageQuery & candidates,
        const libdnf5::advisory::AdvisoryQuery & advisories,
        bool add_obsoletes);

    void add_group_install_to_goal(
        base::Transaction & transaction,
        const transaction::TransactionItemReason reason,
        comps::GroupQuery group_query,
        GoalJobSettings & settings);
    void add_group_remove_to_goal(
        std::vector<std::tuple<std::string, transaction::TransactionItemReason, comps::GroupQuery, GoalJobSettings>> &
            groups_to_remove);
    void add_group_upgrade_to_goal(
        base::Transaction & transaction, comps::GroupQuery group_query, GoalJobSettings & settings);

    void add_environment_install_to_goal(
        base::Transaction & transaction, comps::EnvironmentQuery environment_query, GoalJobSettings & settings);
    void add_environment_remove_to_goal(
        base::Transaction & transaction,
        std::vector<std::tuple<std::string, comps::EnvironmentQuery, GoalJobSettings>> & environments_to_remove);
    void add_environment_upgrade_to_goal(
        base::Transaction & transaction, comps::EnvironmentQuery environment_query, GoalJobSettings & settings);

    GoalProblem add_reason_change_to_goal(
        base::Transaction & transaction,
        const std::string & spec,
        const transaction::TransactionItemReason reason,
        const std::optional<std::string> & group_id,
        GoalJobSettings & settings);

    /// Parse the spec (package, group, remote or local rpm file) and process it.
    /// Repository packages and groups are directly added to rpm_specs / group_specs,
    /// files are stored for being later downloaded and added to command line repo.
    void add_spec(GoalAction action, const std::string & spec, const GoalJobSettings & settings);

    /// Add all (remote or local) rpm paths to the goal.
    /// Remote URLs are first downloaded and all the paths are inserted into
    /// cmdline repo.
    void add_paths_to_goal();

    void set_exclude_from_weak(const std::vector<std::string> & exclude_from_weak);
    void autodetect_unsatisfied_installed_weak_dependencies();

private:
    friend class Goal;
    BaseWeakPtr base;
    std::vector<std::string> module_enable_specs;
    /// <libdnf5::GoalAction, std::string pkg_spec, libdnf5::GoalJobSettings settings>
    std::vector<std::tuple<GoalAction, std::string, GoalJobSettings>> rpm_specs;
    /// <TransactionItemReason reason, std::string pkg_spec, optional<std::string> group id, libdnf5::GoalJobSettings settings>
    std::vector<std::tuple<
        libdnf5::transaction::TransactionItemReason,
        std::string,
        std::optional<std::string>,
        GoalJobSettings>>
        rpm_reason_change_specs;
    /// <libdnf5::GoalAction, rpm Ids, libdnf5::GoalJobSettings settings>
    std::vector<std::tuple<GoalAction, libdnf5::solv::IdQueue, GoalJobSettings>> rpm_ids;
    /// <libdnf5::GoalAction, std::string filepath, libdnf5::GoalJobSettings settings>
    std::vector<std::tuple<GoalAction, std::string, GoalJobSettings>> rpm_filepaths;

    // (spec, reason, query, settings)
    using GroupItem = std::tuple<std::string, transaction::TransactionItemReason, comps::GroupQuery, GoalJobSettings>;
    // To correctly remove all unneeded group packages when a group is removed,
    // the list of all other removed groups in the transaction is needed.
    // Therefore resolve spec -> group_query first.
    std::map<GoalAction, std::vector<GroupItem>> resolved_group_specs;

    using EnvironmentItem = std::tuple<std::string, comps::EnvironmentQuery, GoalJobSettings>;
    std::map<GoalAction, std::vector<EnvironmentItem>> resolved_environment_specs;

    /// group_specs contain both comps groups and environments. These are resolved
    /// according to settings.group_search_type flags value.
    /// <libdnf5::GoalAction, TransactionItemReason reason, std::string group_spec, GoalJobSettings settings>
    std::vector<GroupSpec> group_specs;

    rpm::solv::GoalPrivate rpm_goal;
    bool allow_erasing{false};

    void install_group_package(base::Transaction & transaction, libdnf5::comps::Package pkg);
    void remove_group_packages(const rpm::PackageSet & remove_candidates);
};

Goal::Goal(const BaseWeakPtr & base) : p_impl(new Impl(base)) {}
Goal::Goal(Base & base) : p_impl(new Impl(base.get_weak_ptr())) {}

Goal::Impl::Impl(const BaseWeakPtr & base) : base(base), rpm_goal(base) {}

Goal::~Goal() = default;

Goal::Impl::~Impl() = default;

void Goal::add_module_enable(const std::string & spec) {
    p_impl->module_enable_specs.push_back(spec);
}

void Goal::add_install(const std::string & spec, const libdnf5::GoalJobSettings & settings) {
    p_impl->add_spec(GoalAction::INSTALL, spec, settings);
}

void Goal::add_upgrade(const std::string & spec, const libdnf5::GoalJobSettings & settings, bool minimal) {
    p_impl->add_spec(minimal ? GoalAction::UPGRADE_MINIMAL : GoalAction::UPGRADE, spec, settings);
}

void Goal::add_downgrade(const std::string & spec, const libdnf5::GoalJobSettings & settings) {
    p_impl->add_spec(GoalAction::DOWNGRADE, spec, settings);
}

void Goal::add_reinstall(const std::string & spec, const GoalJobSettings & settings) {
    p_impl->add_spec(GoalAction::REINSTALL, spec, settings);
}

void Goal::add_remove(const std::string & spec, const GoalJobSettings & settings) {
    p_impl->add_spec(GoalAction::REMOVE, spec, settings);
}


void Goal::add_rpm_install(const std::string & spec, const GoalJobSettings & settings) {
    p_impl->rpm_specs.push_back(std::make_tuple(GoalAction::INSTALL, spec, settings));
}

void Goal::add_rpm_install(const rpm::Package & rpm_package, const GoalJobSettings & settings) {
    p_impl->add_rpm_ids(GoalAction::INSTALL, rpm_package, settings);
}

void Goal::add_rpm_install(const rpm::PackageSet & package_set, const GoalJobSettings & settings) {
    p_impl->add_rpm_ids(GoalAction::INSTALL, package_set, settings);
}

void Goal::add_rpm_install_or_reinstall(const rpm::Package & rpm_package, const GoalJobSettings & settings) {
    p_impl->add_rpm_ids(GoalAction::INSTALL_OR_REINSTALL, rpm_package, settings);
}

void Goal::add_rpm_install_or_reinstall(const rpm::PackageSet & package_set, const GoalJobSettings & settings) {
    p_impl->add_rpm_ids(GoalAction::INSTALL_OR_REINSTALL, package_set, settings);
}

void Goal::add_rpm_reinstall(const std::string & spec, const GoalJobSettings & settings) {
    p_impl->rpm_specs.push_back(std::make_tuple(GoalAction::REINSTALL, spec, settings));
}

void Goal::add_rpm_reinstall(const rpm::Package & rpm_package, const GoalJobSettings & settings) {
    p_impl->add_rpm_ids(GoalAction::REINSTALL, rpm_package, settings);
}

void Goal::add_rpm_remove(const std::string & spec, const GoalJobSettings & settings) {
    p_impl->rpm_specs.push_back(std::make_tuple(GoalAction::REMOVE, spec, settings));
}

void Goal::add_rpm_remove(const rpm::Package & rpm_package, const GoalJobSettings & settings) {
    p_impl->add_rpm_ids(GoalAction::REMOVE, rpm_package, settings);
}

void Goal::add_rpm_remove(const rpm::PackageSet & package_set, const GoalJobSettings & settings) {
    p_impl->add_rpm_ids(GoalAction::REMOVE, package_set, settings);
}

void Goal::add_rpm_upgrade(const std::string & spec, const GoalJobSettings & settings, bool minimal) {
    if (minimal) {
        p_impl->rpm_specs.push_back(std::make_tuple(GoalAction::UPGRADE_MINIMAL, spec, settings));
    } else {
        p_impl->rpm_specs.push_back(std::make_tuple(GoalAction::UPGRADE, spec, settings));
    }
}

void Goal::add_rpm_upgrade(const GoalJobSettings & settings, bool minimal) {
    if (minimal) {
        p_impl->rpm_specs.push_back(std::make_tuple(GoalAction::UPGRADE_ALL_MINIMAL, std::string(), settings));
    } else {
        p_impl->rpm_specs.push_back(std::make_tuple(GoalAction::UPGRADE_ALL, std::string(), settings));
    }
}

void Goal::add_rpm_upgrade(const rpm::Package & rpm_package, const GoalJobSettings & settings, bool minimal) {
    if (minimal) {
        p_impl->add_rpm_ids(GoalAction::UPGRADE_MINIMAL, rpm_package, settings);
    } else {
        p_impl->add_rpm_ids(GoalAction::UPGRADE, rpm_package, settings);
    }
}

void Goal::add_rpm_upgrade(const rpm::PackageSet & package_set, const GoalJobSettings & settings, bool minimal) {
    if (minimal) {
        p_impl->add_rpm_ids(GoalAction::UPGRADE_MINIMAL, package_set, settings);
    } else {
        p_impl->add_rpm_ids(GoalAction::UPGRADE, package_set, settings);
    }
}

void Goal::add_rpm_downgrade(const std::string & spec, const GoalJobSettings & settings) {
    p_impl->rpm_specs.push_back(std::make_tuple(GoalAction::DOWNGRADE, spec, settings));
}

void Goal::add_rpm_downgrade(const rpm::Package & rpm_package, const GoalJobSettings & settings) {
    p_impl->add_rpm_ids(GoalAction::DOWNGRADE, rpm_package, settings);
}

void Goal::add_rpm_distro_sync(const std::string & spec, const GoalJobSettings & settings) {
    p_impl->rpm_specs.push_back(std::make_tuple(GoalAction::DISTRO_SYNC, spec, settings));
}

void Goal::add_rpm_distro_sync(const GoalJobSettings & settings) {
    p_impl->rpm_specs.push_back(std::make_tuple(GoalAction::DISTRO_SYNC_ALL, std::string(), settings));
}

void Goal::add_rpm_distro_sync(const rpm::Package & rpm_package, const GoalJobSettings & settings) {
    p_impl->add_rpm_ids(GoalAction::DISTRO_SYNC, rpm_package, settings);
}

void Goal::add_rpm_distro_sync(const rpm::PackageSet & package_set, const GoalJobSettings & settings) {
    p_impl->add_rpm_ids(GoalAction::DISTRO_SYNC, package_set, settings);
}

void Goal::add_rpm_reason_change(
    const std::string & spec,
    const libdnf5::transaction::TransactionItemReason reason,
    const std::string & group_id,
    const libdnf5::GoalJobSettings & settings) {
    libdnf_user_assert(
        reason != libdnf5::transaction::TransactionItemReason::GROUP || !group_id.empty(),
        "group_id is required for setting reason \"GROUP\"");
    p_impl->rpm_reason_change_specs.push_back(std::make_tuple(reason, spec, group_id, settings));
}

void Goal::add_provide_install(const std::string & spec, const GoalJobSettings & settings) {
    p_impl->rpm_specs.push_back(std::make_tuple(GoalAction::INSTALL_VIA_PROVIDE, spec, settings));
}

void Goal::Impl::add_spec(GoalAction action, const std::string & spec, const GoalJobSettings & settings) {
    if (spec.starts_with("@")) {
        // spec is a group, environment or a module
        // TODO(mblaha): detect and process modules
        std::string group_spec = spec.substr(1);
        auto group_settings = libdnf5::GoalJobSettings(settings);
        // for compatibility reasons '@group-spec' can mean also environment
        group_settings.group_search_environments = true;
        // support for kickstart environmental groups syntax - '@^environment-spec'
        if (group_spec.starts_with("^")) {
            group_spec = group_spec.substr(1);
            group_settings.group_search_groups = false;
        } else {
            group_settings.group_search_groups = true;
        }
        group_specs.push_back(
            std::make_tuple(action, libdnf5::transaction::TransactionItemReason::USER, group_spec, group_settings));
    } else {
        const std::string_view ext(".rpm");
        if (libdnf5::utils::url::is_url(spec) || (spec.length() > ext.length() && spec.ends_with(ext))) {
            // spec is a remote rpm file or a local rpm file
            if (action == GoalAction::REMOVE) {
                throw RuntimeError(M_("Unsupported argument for REMOVE action: {}"), spec);
            }
            rpm_filepaths.emplace_back(action, spec, settings);
        } else {
            // otherwise the spec is a repository package
            rpm_specs.emplace_back(action, spec, settings);
        }
    }
}

void Goal::Impl::add_rpm_ids(GoalAction action, const rpm::Package & rpm_package, const GoalJobSettings & settings) {
    libdnf_assert_same_base(base, rpm_package.base);

    libdnf5::solv::IdQueue ids;
    ids.push_back(rpm_package.get_id().id);
    rpm_ids.push_back(std::make_tuple(action, std::move(ids), settings));
}

void Goal::Impl::add_rpm_ids(GoalAction action, const rpm::PackageSet & package_set, const GoalJobSettings & settings) {
    libdnf_assert_same_base(base, package_set.get_base());

    libdnf5::solv::IdQueue ids;
    for (auto package_id : *package_set.p_impl) {
        ids.push_back(package_id);
    }
    rpm_ids.push_back(std::make_tuple(action, std::move(ids), settings));
}

// @replaces part of libdnf/sack/query.cpp:method:filterAdvisory called with HY_EQG and HY_UPGRADE
void Goal::Impl::filter_candidates_for_advisory_upgrade(
    const BaseWeakPtr & base,
    libdnf5::rpm::PackageQuery & candidates,
    const libdnf5::advisory::AdvisoryQuery & advisories,
    bool add_obsoletes) {
    rpm::PackageQuery installed(base);
    installed.filter_installed();

    // When doing advisory upgrade consider only candidate pkgs that can possibly upgrade some pkg.
    // Both branches do candidates.filter_upgrades().
    // This basically means that it matches some already installed pkg by name, has higher evr and
    // has the same architecture or one of them is noarch.
    // This is required because otherwise a pkg with different Arch than installed or noarch can end
    // up in upgrade set which is wrong. It can result in dependency issues, reported as: RhBug:2088149.
    if (add_obsoletes) {
        rpm::PackageQuery obsoletes(candidates);

        candidates.filter_upgrades();

        obsoletes.filter_available();
        // Prepare obsoletes of installed as well as obsoletes of any possible upgrade that could happen (candidates)
        rpm::PackageQuery possibly_obsoleted(candidates);
        possibly_obsoleted |= installed;
        obsoletes.filter_obsoletes(possibly_obsoleted);

        // Add obsoletes to candidates
        candidates |= obsoletes;
    } else {
        candidates.filter_upgrades();
    }

    // Apply security filters only to packages with highest priority (lowest priority number),
    // to unify behaviour of upgrade and upgrade-minimal
    candidates.filter_priority();

    // Since we want to satisfy all advisory packages we can keep just the latest
    // (all lower EVR adv pkgs are satistified by the latests)
    // We also want to skip already resolved advisories.
    candidates.filter_latest_unresolved_advisories(advisories, installed, libdnf5::sack::QueryCmp::GTE);
}

void Goal::add_group_install(
    const std::string & spec,
    const libdnf5::transaction::TransactionItemReason reason,
    const GoalJobSettings & settings) {
    p_impl->group_specs.push_back(std::make_tuple(GoalAction::INSTALL, reason, spec, settings));
}

void Goal::add_group_remove(
    const std::string & spec,
    const libdnf5::transaction::TransactionItemReason reason,
    const GoalJobSettings & settings) {
    p_impl->group_specs.push_back(std::make_tuple(GoalAction::REMOVE, reason, spec, settings));
}

void Goal::add_group_upgrade(const std::string & spec, const libdnf5::GoalJobSettings & settings) {
    // upgrade keeps old reason, thus use NONE here
    p_impl->group_specs.push_back(
        std::make_tuple(GoalAction::UPGRADE, libdnf5::transaction::TransactionItemReason::NONE, spec, settings));
}

GoalProblem Goal::Impl::add_specs_to_goal(base::Transaction & transaction) {
    auto sack = base->get_rpm_package_sack();
    auto & cfg_main = base->get_config();
    auto ret = GoalProblem::NO_PROBLEM;
    for (auto & [action, spec, settings] : rpm_specs) {
        switch (action) {
            case GoalAction::INSTALL:
            case GoalAction::INSTALL_BY_COMPS: {
                auto [problem, idqueue] = add_install_to_goal(transaction, action, spec, settings);
                rpm_goal.add_transaction_user_installed(idqueue);
                ret |= problem;
            } break;
            case GoalAction::INSTALL_VIA_PROVIDE:
                add_provide_install_to_goal(spec, settings);
                break;
            case GoalAction::REINSTALL:
                ret |= add_reinstall_to_goal(transaction, spec, settings);
                break;
            case GoalAction::REMOVE:
                add_remove_to_goal(transaction, spec, settings);
                break;
            case GoalAction::DISTRO_SYNC:
            case GoalAction::DOWNGRADE:
            case GoalAction::UPGRADE:
                ret |= add_up_down_distrosync_to_goal(transaction, action, spec, settings);
                break;
            case GoalAction::UPGRADE_MINIMAL:
                ret |= add_up_down_distrosync_to_goal(transaction, action, spec, settings, true);
                break;
            case GoalAction::UPGRADE_ALL:
            case GoalAction::UPGRADE_ALL_MINIMAL: {
                rpm::PackageQuery query(base);

                // Apply advisory filters
                if (settings.advisory_filter.has_value()) {
                    filter_candidates_for_advisory_upgrade(
                        base, query, settings.advisory_filter.value(), cfg_main.get_obsoletes_option().get_value());
                }

                // Make the smallest possible upgrade
                if (action == GoalAction::UPGRADE_ALL_MINIMAL) {
                    query.filter_earliest_evr();
                }

                libdnf5::solv::IdQueue upgrade_ids;
                for (auto package_id : *query.p_impl) {
                    upgrade_ids.push_back(package_id);
                }
                rpm_goal.add_upgrade(
                    upgrade_ids, settings.resolve_best(cfg_main), settings.resolve_clean_requirements_on_remove());
            } break;
            case GoalAction::DISTRO_SYNC_ALL: {
                rpm::PackageQuery query(base);
                libdnf5::solv::IdQueue upgrade_ids;
                for (auto package_id : *query.p_impl) {
                    upgrade_ids.push_back(package_id);
                }
                rpm_goal.add_distro_sync(
                    upgrade_ids,
                    settings.resolve_skip_broken(cfg_main),
                    settings.resolve_best(cfg_main),
                    settings.resolve_clean_requirements_on_remove());
            } break;
            case GoalAction::INSTALL_OR_REINSTALL: {
                libdnf_throw_assertion("Unsupported action \"INSTALL_OR_REINSTALL\"");
            }
            case GoalAction::RESOLVE: {
                libdnf_throw_assertion("Unsupported action \"RESOLVE\"");
            }
            case GoalAction::REASON_CHANGE: {
                libdnf_throw_assertion("Unsupported action \"REASON_CHANGE\"");
            }
        }
    }
    return ret;
}

GoalProblem Goal::Impl::resolve_group_specs(std::vector<GroupSpec> & specs, base::Transaction & transaction) {
    auto ret = GoalProblem::NO_PROBLEM;
    auto & cfg_main = base->get_config();
    // optimization - creating a group query from scratch is relatively expensive,
    // but the copy construction is cheap, so prepare a base query to be copied.
    comps::GroupQuery base_groups_query(base);
    comps::EnvironmentQuery base_environments_query(base);
    for (auto & [action, reason, spec, settings] : specs) {
        bool skip_unavailable = settings.resolve_skip_unavailable(cfg_main);
        auto log_level = skip_unavailable ? libdnf5::Logger::Level::WARNING : libdnf5::Logger::Level::ERROR;
        if (action != GoalAction::INSTALL && action != GoalAction::INSTALL_BY_COMPS && action != GoalAction::REMOVE &&
            action != GoalAction::UPGRADE) {
            transaction.p_impl->add_resolve_log(
                action,
                GoalProblem::UNSUPPORTED_ACTION,
                settings,
                libdnf5::transaction::TransactionItemType::GROUP,
                spec,
                {},
                log_level);
            ret |= GoalProblem::UNSUPPORTED_ACTION;
            continue;
        }
        sack::QueryCmp cmp = settings.ignore_case ? sack::QueryCmp::IGLOB : sack::QueryCmp::GLOB;
        bool spec_resolved{false};
        if (settings.group_search_groups) {
            comps::GroupQuery group_query(base, true);
            comps::GroupQuery spec_groups_query(base_groups_query);
            // for REMOVE / UPGRADE actions take only installed groups into account
            // for INSTALL only available groups
            spec_groups_query.filter_installed(action != GoalAction::INSTALL && action != GoalAction::INSTALL_BY_COMPS);
            if (settings.group_with_id) {
                comps::GroupQuery group_query_id(spec_groups_query);
                group_query_id.filter_groupid(spec, cmp);
                group_query |= group_query_id;
            }
            // TODO(mblaha): reconsider usefulness of searching groups by names
            if (settings.group_with_name) {
                comps::GroupQuery group_query_name(spec_groups_query);
                group_query_name.filter_name(spec, cmp);
                group_query |= group_query_name;
            }
            if (!group_query.empty()) {
                resolved_group_specs[action].push_back({spec, reason, std::move(group_query), settings});
                spec_resolved = true;
            }
        }
        if (settings.group_search_environments) {
            comps::EnvironmentQuery environment_query(base, true);
            comps::EnvironmentQuery spec_environments_query(base_environments_query);
            spec_environments_query.filter_installed(action != GoalAction::INSTALL);
            if (settings.group_with_id) {
                comps::EnvironmentQuery environment_query_id(spec_environments_query);
                environment_query_id.filter_environmentid(spec, cmp);
                environment_query |= environment_query_id;
            }
            // TODO(mblaha): reconsider usefulness of searching groups by names
            if (settings.group_with_name) {
                comps::EnvironmentQuery environment_query_name(spec_environments_query);
                environment_query_name.filter_name(spec, cmp);
                environment_query |= environment_query_name;
            }
            if (!environment_query.empty()) {
                resolved_environment_specs[action].push_back({spec, std::move(environment_query), settings});
                spec_resolved = true;
            }
        }
        if (!spec_resolved) {
            transaction.p_impl->add_resolve_log(
                action,
                GoalProblem::NOT_FOUND,
                settings,
                libdnf5::transaction::TransactionItemType::GROUP,
                spec,
                {},
                log_level);
            if (!skip_unavailable) {
                ret |= GoalProblem::NOT_FOUND;
            }
        }
    }

    return ret;
}

void Goal::Impl::add_resolved_environment_specs_to_goal(base::Transaction & transaction) {
    add_environment_remove_to_goal(transaction, resolved_environment_specs[GoalAction::REMOVE]);

    for (auto & [spec, environment_query, settings] : resolved_environment_specs[GoalAction::INSTALL]) {
        add_environment_install_to_goal(transaction, environment_query, settings);
    }

    for (auto & [spec, environment_query, settings] : resolved_environment_specs[GoalAction::UPGRADE]) {
        add_environment_upgrade_to_goal(transaction, environment_query, settings);
    }
}

void Goal::Impl::add_resolved_group_specs_to_goal(base::Transaction & transaction) {
    // process group removals first
    add_group_remove_to_goal(resolved_group_specs[GoalAction::REMOVE]);

    for (const auto & action : std::vector<GoalAction>{GoalAction::INSTALL, GoalAction::INSTALL_BY_COMPS}) {
        for (auto & [spec, reason, group_query, settings] : resolved_group_specs[action]) {
            add_group_install_to_goal(transaction, reason, group_query, settings);
        }
    }

    for (auto & [spec, reason, group_query, settings] : resolved_group_specs[GoalAction::UPGRADE]) {
        add_group_upgrade_to_goal(transaction, group_query, settings);
    }
}


GoalProblem Goal::Impl::add_reason_change_specs_to_goal(base::Transaction & transaction) {
    auto ret = GoalProblem::NO_PROBLEM;
    for (auto & [reason, spec, group_id, settings] : rpm_reason_change_specs) {
        ret |= add_reason_change_to_goal(transaction, spec, reason, group_id, settings);
    }
    return ret;
}

std::pair<GoalProblem, libdnf5::solv::IdQueue> Goal::Impl::add_install_to_goal(
    base::Transaction & transaction, GoalAction action, const std::string & spec, GoalJobSettings & settings) {
    auto sack = base->get_rpm_package_sack();
    auto & pool = get_rpm_pool(base);
    auto & cfg_main = base->get_config();
    bool skip_unavailable = settings.resolve_skip_unavailable(cfg_main);
    auto log_level = skip_unavailable ? libdnf5::Logger::Level::WARNING : libdnf5::Logger::Level::ERROR;
    bool best = settings.resolve_best(cfg_main);
    bool clean_requirements_on_remove = settings.resolve_clean_requirements_on_remove();

    auto multilib_policy = cfg_main.get_multilib_policy_option().get_value();
    libdnf5::solv::IdQueue result_queue;
    rpm::PackageQuery base_query(base);

    rpm::PackageQuery query(base_query);
    auto nevra_pair = query.resolve_pkg_spec(spec, settings, false);
    if (!nevra_pair.first) {
        auto problem = transaction.p_impl->report_not_found(action, spec, settings, log_level);
        if (skip_unavailable) {
            return {GoalProblem::NO_PROBLEM, result_queue};
        } else {
            return {problem, result_queue};
        }
    }

    // The correct evaluation of rich dependencies can be only performed by solver.
    // There are some limitations - solver is unable to handle whan operation is limited to packages from the
    // particular repository and multilib_policy `all`.
    if (libdnf5::rpm::Reldep::is_rich_dependency(spec) && settings.to_repo_ids.empty()) {
        add_provide_install_to_goal(spec, settings);
        return {GoalProblem::NO_PROBLEM, result_queue};
    }

    bool has_just_name = nevra_pair.second.has_just_name();
    bool add_obsoletes = cfg_main.get_obsoletes_option().get_value() && has_just_name;

    rpm::PackageQuery installed(query);
    installed.filter_installed();
    for (auto package_id : *installed.p_impl) {
        transaction.p_impl->add_resolve_log(
            action,
            GoalProblem::ALREADY_INSTALLED,
            settings,
            libdnf5::transaction::TransactionItemType::PACKAGE,
            spec,
            {pool.get_nevra(package_id)},
            libdnf5::Logger::Level::WARNING);
    }

    bool skip_broken = settings.resolve_skip_broken(cfg_main);

    if (multilib_policy == "all" || utils::is_glob_pattern(nevra_pair.second.get_arch().c_str())) {
        if (!settings.to_repo_ids.empty()) {
            query.filter_repo_id(settings.to_repo_ids, sack::QueryCmp::GLOB);
            if (query.empty()) {
                transaction.p_impl->add_resolve_log(
                    action,
                    GoalProblem::NOT_FOUND_IN_REPOSITORIES,
                    settings,
                    libdnf5::transaction::TransactionItemType::PACKAGE,
                    spec,
                    {},
                    log_level);
                return {GoalProblem::NOT_FOUND_IN_REPOSITORIES, result_queue};
            }
            query |= installed;
        }

        // Apply advisory filters
        if (settings.advisory_filter.has_value()) {
            query.filter_advisories(settings.advisory_filter.value(), libdnf5::sack::QueryCmp::EQ);
        }

        /// <name, <arch, std::vector<pkg Solvables>>>
        std::unordered_map<Id, std::unordered_map<Id, std::vector<Solvable *>>> na_map;

        for (auto package_id : *query.p_impl) {
            Solvable * solvable = pool.id2solvable(package_id);
            na_map[solvable->name][solvable->arch].push_back(solvable);
        }

        rpm::PackageSet selected(base);
        rpm::PackageSet selected_noarch(base);
        for (auto & name_iter : na_map) {
            if (name_iter.second.size() == 1) {
                selected.clear();
                for (auto * solvable : name_iter.second.begin()->second) {
                    selected.p_impl->add(pool.solvable2id(solvable));
                }
                if (add_obsoletes) {
                    add_obsoletes_to_data(base_query, selected);
                }
                solv_map_to_id_queue(result_queue, *selected.p_impl);
                rpm_goal.add_install(result_queue, skip_broken, best, clean_requirements_on_remove);
            } else {
                // when multiple architectures -> add noarch solvables into each architecture solvable set
                auto noarch = name_iter.second.find(ARCH_NOARCH);
                if (noarch != name_iter.second.end()) {
                    selected_noarch.clear();
                    for (auto * solvable : noarch->second) {
                        selected_noarch.p_impl->add(pool.solvable2id(solvable));
                    }
                    if (add_obsoletes) {
                        add_obsoletes_to_data(base_query, selected_noarch);
                    }
                    for (auto & arch_iter : name_iter.second) {
                        if (arch_iter.first == ARCH_NOARCH) {
                            continue;
                        }
                        selected.clear();
                        for (auto * solvable : arch_iter.second) {
                            selected.p_impl->add(pool.solvable2id(solvable));
                        }
                        if (add_obsoletes) {
                            add_obsoletes_to_data(base_query, selected);
                        }
                        selected |= selected_noarch;
                        solv_map_to_id_queue(result_queue, *selected.p_impl);
                        rpm_goal.add_install(result_queue, skip_broken, best, clean_requirements_on_remove);
                    }
                } else {
                    for (auto & arch_iter : name_iter.second) {
                        selected.clear();
                        for (auto * solvable : arch_iter.second) {
                            selected.p_impl->add(pool.solvable2id(solvable));
                        }
                        if (add_obsoletes) {
                            add_obsoletes_to_data(base_query, selected);
                        }
                        solv_map_to_id_queue(result_queue, *selected.p_impl);
                        rpm_goal.add_install(result_queue, skip_broken, best, clean_requirements_on_remove);
                    }
                }
            }
        }
        // TODO(jmracek) Implement all logic for modules and comps groups
    } else if (multilib_policy == "best") {
        if ((!utils::is_file_pattern(spec) && utils::is_glob_pattern(spec.c_str())) ||
            (nevra_pair.second.get_name().empty() &&
             (!nevra_pair.second.get_epoch().empty() || !nevra_pair.second.get_version().empty() ||
              !nevra_pair.second.get_release().empty() || !nevra_pair.second.get_arch().empty()))) {
            if (!settings.to_repo_ids.empty()) {
                query.filter_repo_id(settings.to_repo_ids, sack::QueryCmp::GLOB);
                if (query.empty()) {
                    transaction.p_impl->add_resolve_log(
                        action,
                        GoalProblem::NOT_FOUND_IN_REPOSITORIES,
                        settings,
                        libdnf5::transaction::TransactionItemType::PACKAGE,
                        spec,
                        {},
                        log_level);
                    return {GoalProblem::NOT_FOUND_IN_REPOSITORIES, result_queue};
                }
                query |= installed;
            }

            // Apply advisory filters
            if (settings.advisory_filter.has_value()) {
                query.filter_advisories(settings.advisory_filter.value(), libdnf5::sack::QueryCmp::EQ);
            }

            rpm::PackageQuery available(query);
            available.filter_available();

            // keep only installed that has a partner in available
            std::unordered_set<Id> names;
            for (auto package_id : *available.p_impl) {
                Solvable * solvable = pool.id2solvable(package_id);
                names.insert(solvable->name);
            }
            for (auto package_id : *installed.p_impl) {
                Solvable * solvable = pool.id2solvable(package_id);
                auto name_iterator = names.find(solvable->name);
                if (name_iterator == names.end()) {
                    installed.p_impl->remove_unsafe(package_id);
                }
            }
            available |= installed;
            std::vector<Solvable *> tmp_solvables;
            for (auto package_id : *available.p_impl) {
                Solvable * solvable = pool.id2solvable(package_id);
                tmp_solvables.push_back(solvable);
            }

            rpm::PackageSet selected(base);
            if (!tmp_solvables.empty()) {
                Id current_name = 0;
                std::sort(tmp_solvables.begin(), tmp_solvables.end(), nevra_solvable_cmp_key);
                auto * first = tmp_solvables[0];
                current_name = first->name;
                selected.p_impl->add_unsafe(pool.solvable2id(first));

                for (auto iter = std::next(tmp_solvables.begin()); iter != tmp_solvables.end(); ++iter) {
                    if ((*iter)->name == current_name) {
                        selected.p_impl->add_unsafe(pool.solvable2id(*iter));
                        continue;
                    }
                    if (add_obsoletes) {
                        add_obsoletes_to_data(base_query, selected);
                    }
                    solv_map_to_id_queue(result_queue, static_cast<libdnf5::solv::SolvMap>(*selected.p_impl));
                    rpm_goal.add_install(result_queue, skip_broken, best, clean_requirements_on_remove);
                    selected.clear();
                    selected.p_impl->add_unsafe(pool.solvable2id(*iter));
                    current_name = (*iter)->name;
                }
            }
            if (add_obsoletes) {
                add_obsoletes_to_data(base_query, selected);
            }
            solv_map_to_id_queue(result_queue, static_cast<libdnf5::solv::SolvMap>(*selected.p_impl));
            rpm_goal.add_install(result_queue, skip_broken, best, clean_requirements_on_remove);
        } else {
            if (add_obsoletes) {
                add_obsoletes_to_data(base_query, query);
            }
            if (!settings.to_repo_ids.empty()) {
                query.filter_repo_id(settings.to_repo_ids, sack::QueryCmp::GLOB);
                if (query.empty()) {
                    transaction.p_impl->add_resolve_log(
                        action,
                        GoalProblem::NOT_FOUND_IN_REPOSITORIES,
                        settings,
                        libdnf5::transaction::TransactionItemType::PACKAGE,
                        spec,
                        {},
                        log_level);
                    return {GoalProblem::NOT_FOUND_IN_REPOSITORIES, result_queue};
                }
                query |= installed;
            }

            // Apply advisory filters
            if (settings.advisory_filter.has_value()) {
                query.filter_advisories(settings.advisory_filter.value(), libdnf5::sack::QueryCmp::EQ);
            }
            solv_map_to_id_queue(result_queue, *query.p_impl);
            rpm_goal.add_install(result_queue, skip_broken, best, clean_requirements_on_remove);
        }
    } else {
        // TODO(lukash) throw a proper exception
        throw RuntimeError(M_("Incorrect configuration value for multilib_policy: {}"), multilib_policy);
    }

    return {GoalProblem::NO_PROBLEM, result_queue};
}

void Goal::Impl::add_provide_install_to_goal(const std::string & spec, GoalJobSettings & settings) {
    auto & cfg_main = base->get_config();
    bool skip_broken = settings.resolve_skip_broken(cfg_main);
    bool best = settings.resolve_best(cfg_main);
    bool clean_requirements_on_remove = settings.resolve_clean_requirements_on_remove();
    rpm::Reldep reldep(base, spec);
    rpm_goal.add_provide_install(reldep.get_id(), skip_broken, best, clean_requirements_on_remove);
}

GoalProblem Goal::Impl::add_reinstall_to_goal(
    base::Transaction & transaction, const std::string & spec, GoalJobSettings & settings) {
    // Resolve all settings before the first report => they will be storred in settings
    auto & cfg_main = base->get_config();
    bool skip_unavailable = settings.resolve_skip_unavailable(cfg_main);
    auto log_level = skip_unavailable ? libdnf5::Logger::Level::WARNING : libdnf5::Logger::Level::ERROR;
    bool best = settings.resolve_best(cfg_main);
    bool clean_requirements_on_remove = settings.resolve_clean_requirements_on_remove();
    auto sack = base->get_rpm_package_sack();
    rpm::PackageQuery query(base);
    auto nevra_pair = query.resolve_pkg_spec(spec, settings, false);
    if (!nevra_pair.first) {
        auto problem = transaction.p_impl->report_not_found(GoalAction::REINSTALL, spec, settings, log_level);
        return skip_unavailable ? GoalProblem::NO_PROBLEM : problem;
    }

    // Report when package is not installed
    rpm::PackageQuery query_installed(query);
    query_installed.filter_installed();
    if (query_installed.empty()) {
        transaction.p_impl->add_resolve_log(
            GoalAction::REINSTALL,
            GoalProblem::NOT_INSTALLED,
            settings,
            libdnf5::transaction::TransactionItemType::PACKAGE,
            spec,
            {},
            log_level);
        return skip_unavailable ? GoalProblem::NO_PROBLEM : GoalProblem::NOT_INSTALLED;
    }

    // keep only available packages
    query -= query_installed;
    if (query.empty()) {
        transaction.p_impl->add_resolve_log(
            GoalAction::REINSTALL,
            GoalProblem::NOT_AVAILABLE,
            settings,
            libdnf5::transaction::TransactionItemType::PACKAGE,
            spec,
            {},
            log_level);
        return skip_unavailable ? GoalProblem::NO_PROBLEM : GoalProblem::NOT_AVAILABLE;
    }

    // keeps only available packages that are installed with same NEVRA
    rpm::PackageQuery relevant_available(query);
    relevant_available.filter_nevra(query_installed);
    if (relevant_available.empty()) {
        rpm::PackageQuery relevant_available_na(query);
        relevant_available_na.filter_name_arch(query_installed);
        if (!relevant_available_na.empty()) {
            transaction.p_impl->add_resolve_log(
                GoalAction::REINSTALL,
                GoalProblem::INSTALLED_IN_DIFFERENT_VERSION,
                settings,
                libdnf5::transaction::TransactionItemType::PACKAGE,
                spec,
                {},
                log_level);
            return skip_unavailable ? GoalProblem::NO_PROBLEM : GoalProblem::INSTALLED_IN_DIFFERENT_VERSION;
        } else {
            rpm::PackageQuery relevant_available_n(query);
            relevant_available_n.filter_name(query_installed);
            if (relevant_available_n.empty()) {
                transaction.p_impl->add_resolve_log(
                    GoalAction::REINSTALL,
                    GoalProblem::NOT_INSTALLED,
                    settings,
                    libdnf5::transaction::TransactionItemType::PACKAGE,
                    spec,
                    {},
                    log_level);
                return skip_unavailable ? GoalProblem::NO_PROBLEM : GoalProblem::NOT_INSTALLED;
            } else {
                transaction.p_impl->add_resolve_log(
                    GoalAction::REINSTALL,
                    GoalProblem::NOT_INSTALLED_FOR_ARCHITECTURE,
                    settings,
                    libdnf5::transaction::TransactionItemType::PACKAGE,
                    spec,
                    {},
                    log_level);
                return skip_unavailable ? GoalProblem::NO_PROBLEM : GoalProblem::NOT_INSTALLED_FOR_ARCHITECTURE;
            }
        }
    }

    // TODO(jmracek) Implement fitering from_repo_ids

    if (!settings.to_repo_ids.empty()) {
        relevant_available.filter_repo_id(settings.to_repo_ids, sack::QueryCmp::GLOB);
        if (relevant_available.empty()) {
            transaction.p_impl->add_resolve_log(
                GoalAction::REINSTALL,
                GoalProblem::NOT_FOUND_IN_REPOSITORIES,
                settings,
                libdnf5::transaction::TransactionItemType::PACKAGE,
                spec,
                {},
                log_level);
            return skip_unavailable ? GoalProblem::NO_PROBLEM : GoalProblem::NOT_FOUND_IN_REPOSITORIES;
        }
    }

    Id current_name = 0;
    Id current_arch = 0;
    std::vector<Solvable *> tmp_solvables;
    auto & pool = get_rpm_pool(base);

    for (auto package_id : *relevant_available.p_impl) {
        tmp_solvables.push_back(pool.id2solvable(package_id));
    }
    std::sort(tmp_solvables.begin(), tmp_solvables.end(), nevra_solvable_cmp_key);

    libdnf5::solv::IdQueue tmp_queue;

    {
        auto * first = (*tmp_solvables.begin());
        current_name = first->name;
        current_arch = first->arch;
        tmp_queue.push_back(pool.solvable2id(first));
    }

    bool skip_broken = settings.resolve_skip_broken(cfg_main);

    for (auto iter = std::next(tmp_solvables.begin()); iter != tmp_solvables.end(); ++iter) {
        if ((*iter)->name == current_name && (*iter)->arch == current_arch) {
            tmp_queue.push_back(pool.solvable2id(*iter));
            continue;
        }
        rpm_goal.add_install(tmp_queue, skip_broken, best, clean_requirements_on_remove);
        tmp_queue.clear();
        tmp_queue.push_back(pool.solvable2id(*iter));
        current_name = (*iter)->name;
        current_arch = (*iter)->arch;
    }
    rpm_goal.add_install(tmp_queue, skip_broken, best, clean_requirements_on_remove);
    return GoalProblem::NO_PROBLEM;
}

void Goal::Impl::add_rpms_to_goal(base::Transaction & transaction) {
    auto sack = base->get_rpm_package_sack();
    auto & pool = get_rpm_pool(base);
    auto & cfg_main = base->get_config();

    rpm::PackageQuery installed(base, rpm::PackageQuery::ExcludeFlags::IGNORE_EXCLUDES);
    installed.filter_installed();
    for (auto [action, ids, settings] : rpm_ids) {
        switch (action) {
            case GoalAction::INSTALL: {
                bool skip_broken = settings.resolve_skip_broken(cfg_main);
                bool skip_unavailable = settings.resolve_skip_unavailable(cfg_main);
                auto log_level = skip_unavailable ? libdnf5::Logger::Level::WARNING : libdnf5::Logger::Level::ERROR;
                bool best = settings.resolve_best(cfg_main);
                bool clean_requirements_on_remove = settings.resolve_clean_requirements_on_remove();
                //  include installed packages with the same NEVRA into transaction to prevent reinstall
                std::vector<std::string> nevras;
                for (auto id : ids) {
                    nevras.push_back(pool.get_nevra(id));
                }
                rpm::PackageQuery query(installed);
                query.filter_nevra(nevras);
                //  report aready installed packages with the same NEVRA
                for (auto package_id : *query.p_impl) {
                    transaction.p_impl->add_resolve_log(
                        action,
                        GoalProblem::ALREADY_INSTALLED,
                        settings,
                        libdnf5::transaction::TransactionItemType::PACKAGE,
                        {},
                        {pool.get_nevra(package_id)},
                        log_level);
                    ids.push_back(package_id);
                }
                rpm_goal.add_install(ids, skip_broken, best, clean_requirements_on_remove);
                rpm_goal.add_transaction_user_installed(ids);
            } break;
            case GoalAction::INSTALL_OR_REINSTALL:
                rpm_goal.add_install(
                    ids,
                    settings.resolve_skip_broken(cfg_main),
                    settings.resolve_best(cfg_main),
                    settings.resolve_clean_requirements_on_remove());
                break;
            case GoalAction::REINSTALL: {
                bool skip_unavailable = settings.resolve_skip_unavailable(cfg_main);
                auto log_level = skip_unavailable ? libdnf5::Logger::Level::WARNING : libdnf5::Logger::Level::ERROR;
                bool skip_broken = settings.resolve_skip_broken(cfg_main);
                bool best = settings.resolve_best(cfg_main);
                bool clean_requirements_on_remove = settings.resolve_clean_requirements_on_remove();
                solv::IdQueue ids_nevra_installed;
                for (auto id : ids) {
                    rpm::PackageQuery query(installed);
                    query.filter_nevra({pool.get_nevra(id)});
                    if (query.empty()) {
                        // Report when package with the same NEVRA is not installed
                        transaction.p_impl->add_resolve_log(
                            action,
                            GoalProblem::NOT_INSTALLED,
                            settings,
                            libdnf5::transaction::TransactionItemType::PACKAGE,
                            {pool.get_nevra(id)},
                            {},
                            log_level);
                    } else {
                        // Only installed packages can be reinstalled
                        ids_nevra_installed.push_back(id);
                    }
                }
                rpm_goal.add_install(ids_nevra_installed, skip_broken, best, clean_requirements_on_remove);
            } break;
            case GoalAction::UPGRADE: {
                bool best = settings.resolve_best(cfg_main);
                bool clean_requirements_on_remove = settings.resolve_clean_requirements_on_remove();
                // TODO(jrohel): Now logs all packages that are not upgrades. It can be confusing in some cases.
                for (auto id : ids) {
                    if (cfg_main.get_obsoletes_option().get_value()) {
                        rpm::PackageQuery query_id(base, rpm::PackageQuery::ExcludeFlags::IGNORE_EXCLUDES, true);
                        query_id.add(rpm::Package(base, rpm::PackageId(id)));
                        query_id.filter_obsoletes(installed);
                        if (!query_id.empty()) {
                            continue;
                        }
                    }
                    rpm::PackageQuery query(installed);
                    query.filter_name({pool.get_name(id)});
                    if (query.empty()) {
                        // Report when package with the same name is not installed
                        transaction.p_impl->add_resolve_log(
                            action,
                            GoalProblem::NOT_INSTALLED,
                            settings,
                            libdnf5::transaction::TransactionItemType::PACKAGE,
                            {pool.get_nevra(id)},
                            {},
                            libdnf5::Logger::Level::WARNING);
                        continue;
                    }
                    std::string arch = pool.get_arch(id);
                    if (arch != "noarch") {
                        query.filter_arch({arch, "noarch"});
                        if (query.empty()) {
                            // Report when package with the same name is installed for a different architecture
                            // Conversion from/to "noarch" is allowed for upgrade.
                            transaction.p_impl->add_resolve_log(
                                action,
                                GoalProblem::NOT_INSTALLED_FOR_ARCHITECTURE,
                                settings,
                                libdnf5::transaction::TransactionItemType::PACKAGE,
                                {pool.get_nevra(id)},
                                {},
                                libdnf5::Logger::Level::WARNING);
                            continue;
                        }
                    }
                    query.filter_evr({pool.get_evr(id)}, sack::QueryCmp::GTE);
                    if (!query.empty()) {
                        // Report when package with higher or equal version is installed
                        transaction.p_impl->add_resolve_log(
                            action,
                            GoalProblem::ALREADY_INSTALLED,
                            settings,
                            libdnf5::transaction::TransactionItemType::PACKAGE,
                            {pool.get_nevra(id)},
                            {pool.get_name(id) + ("." + arch)},
                            libdnf5::Logger::Level::WARNING);
                        // include installed packages with higher or equal version into transaction to prevent downgrade
                        for (auto installed_id : *query.p_impl) {
                            ids.push_back(installed_id);
                        }
                    }
                }
                rpm_goal.add_upgrade(ids, best, clean_requirements_on_remove);
            } break;
            case GoalAction::DOWNGRADE: {
                bool skip_unavailable = settings.resolve_skip_unavailable(cfg_main);
                auto log_level = skip_unavailable ? libdnf5::Logger::Level::WARNING : libdnf5::Logger::Level::ERROR;
                bool skip_broken = settings.resolve_skip_broken(cfg_main);
                bool best = settings.resolve_best(cfg_main);
                bool clean_requirements_on_remove = settings.resolve_clean_requirements_on_remove();
                solv::IdQueue ids_downgrades;
                for (auto id : ids) {
                    rpm::PackageQuery query(installed);
                    query.filter_name({pool.get_name(id)});
                    if (query.empty()) {
                        // Report when package with the same name is not installed
                        transaction.p_impl->add_resolve_log(
                            action,
                            GoalProblem::NOT_INSTALLED,
                            settings,
                            libdnf5::transaction::TransactionItemType::PACKAGE,
                            {pool.get_nevra(id)},
                            {},
                            log_level);
                        continue;
                    }
                    query.filter_arch({pool.get_arch(id)});
                    if (query.empty()) {
                        // Report when package with the same name is installed for a different architecture
                        transaction.p_impl->add_resolve_log(
                            action,
                            GoalProblem::NOT_INSTALLED_FOR_ARCHITECTURE,
                            settings,
                            libdnf5::transaction::TransactionItemType::PACKAGE,
                            {pool.get_nevra(id)},
                            {},
                            log_level);
                        continue;
                    }
                    query.filter_evr({pool.get_evr(id)}, sack::QueryCmp::LTE);
                    if (!query.empty()) {
                        // Report when package with lower or equal version is installed
                        std::string name_arch(pool.get_name(id));
                        name_arch.append(".");
                        name_arch.append(pool.get_arch(id));
                        transaction.p_impl->add_resolve_log(
                            action,
                            GoalProblem::INSTALLED_LOWEST_VERSION,
                            settings,
                            libdnf5::transaction::TransactionItemType::PACKAGE,
                            {pool.get_nevra(id)},
                            {name_arch},
                            log_level);
                        continue;
                    }

                    // Only installed packages with same name, architecture and higher version can be downgraded
                    ids_downgrades.push_back(id);
                }
                rpm_goal.add_install(ids_downgrades, skip_broken, best, clean_requirements_on_remove);
            } break;
            case GoalAction::DISTRO_SYNC: {
                rpm_goal.add_distro_sync(
                    ids,
                    settings.resolve_skip_broken(cfg_main),
                    settings.resolve_best(cfg_main),
                    settings.resolve_clean_requirements_on_remove());
            } break;
            case GoalAction::REMOVE:
                rpm_goal.add_remove(ids, settings.resolve_clean_requirements_on_remove(cfg_main));
                break;
            default:
                throw std::invalid_argument("Unsupported action");
        }
    }
}


void Goal::Impl::add_remove_to_goal(
    base::Transaction & transaction, const std::string & spec, GoalJobSettings & settings) {
    bool clean_requirements_on_remove = settings.resolve_clean_requirements_on_remove(base->get_config());
    rpm::PackageQuery query(base);
    query.filter_installed();

    auto nevra_pair = query.resolve_pkg_spec(spec, settings, false);
    if (!nevra_pair.first) {
        transaction.p_impl->report_not_found(GoalAction::REMOVE, spec, settings, libdnf5::Logger::Level::WARNING);
        return;
    }

    if (!settings.from_repo_ids.empty()) {
        // TODO(jmracek) keep only packages installed from repo_id -requires swdb
        if (query.empty()) {
            // TODO(jmracek) no solution for the spec => mark result - not from repository
            return;
        }
    }
    rpm_goal.add_remove(*query.p_impl, clean_requirements_on_remove);
}

GoalProblem Goal::Impl::add_up_down_distrosync_to_goal(
    base::Transaction & transaction,
    GoalAction action,
    const std::string & spec,
    GoalJobSettings & settings,
    bool minimal) {
    // Get values before the first report to set in GoalJobSettings used values
    bool best = settings.resolve_best(base->get_config());
    bool skip_broken = action == GoalAction::UPGRADE ? true : settings.resolve_skip_broken(base->get_config());
    bool clean_requirements_on_remove = settings.resolve_clean_requirements_on_remove();
    bool skip_unavailable = settings.resolve_skip_unavailable(base->get_config());

    auto sack = base->get_rpm_package_sack();
    rpm::PackageQuery base_query(base);
    auto obsoletes = base->get_config().get_obsoletes_option().get_value();
    libdnf5::solv::IdQueue tmp_queue;
    rpm::PackageQuery query(base_query);
    auto nevra_pair = query.resolve_pkg_spec(spec, settings, false);
    if (!nevra_pair.first) {
        auto problem = transaction.p_impl->report_not_found(action, spec, settings, libdnf5::Logger::Level::WARNING);
        return skip_unavailable ? GoalProblem::NO_PROBLEM : problem;
    }
    // Report when package is not installed
    rpm::PackageQuery all_installed(base, rpm::PackageQuery::ExcludeFlags::IGNORE_EXCLUDES);
    all_installed.filter_installed();
    // Report only not installed if not obsoleters - https://bugzilla.redhat.com/show_bug.cgi?id=1818118
    bool obsoleters = false;
    if (obsoletes && action != GoalAction::DOWNGRADE) {
        rpm::PackageQuery obsoleters_query(query);
        obsoleters_query.filter_obsoletes(all_installed);
        if (!obsoleters_query.empty()) {
            obsoleters = true;
        }
    }
    rpm::PackageQuery relevant_installed_na(all_installed);
    if (!obsoleters) {
        relevant_installed_na.filter_name_arch(query);
        if (relevant_installed_na.empty()) {
            rpm::PackageQuery relevant_installed_n(all_installed);
            relevant_installed_n.filter_name(query);
            if (relevant_installed_n.empty()) {
                transaction.p_impl->add_resolve_log(
                    action,
                    GoalProblem::NOT_INSTALLED,
                    settings,
                    libdnf5::transaction::TransactionItemType::PACKAGE,
                    spec,
                    {},
                    libdnf5::Logger::Level::WARNING);
                return skip_unavailable ? GoalProblem::NO_PROBLEM : GoalProblem::NOT_INSTALLED;
            }
            transaction.p_impl->add_resolve_log(
                action,
                GoalProblem::NOT_INSTALLED_FOR_ARCHITECTURE,
                settings,
                libdnf5::transaction::TransactionItemType::PACKAGE,
                spec,
                {},
                libdnf5::Logger::Level::WARNING);
            return skip_unavailable ? GoalProblem::NO_PROBLEM : GoalProblem::NOT_INSTALLED_FOR_ARCHITECTURE;
        }
    }

    bool add_obsoletes = obsoletes && nevra_pair.second.has_just_name() && action != GoalAction::DOWNGRADE;
    rpm::PackageQuery installed(query);
    installed.filter_installed();
    // TODO(jmracek) Apply latest filters on installed (or later)
    if (add_obsoletes) {
        // Obsoletes are not added to downgrade set
        if (action == GoalAction::UPGRADE) {
            // Do not add obsoleters of packages that are not upgrades. Such packages can be confusing for the solver.
            rpm::PackageQuery obsoletes_query(base_query);
            obsoletes_query.filter_available();
            rpm::PackageQuery to_obsolete_query(query);
            to_obsolete_query.filter_upgrades();
            to_obsolete_query |= installed;
            obsoletes_query.filter_obsoletes(to_obsolete_query);
            query |= obsoletes_query;
        } else if (action == GoalAction::DISTRO_SYNC) {
            rpm::PackageQuery obsoletes_query(base_query);
            obsoletes_query.filter_available();
            obsoletes_query.filter_obsoletes(query);
            query |= obsoletes_query;
        }
    }
    if (!settings.to_repo_ids.empty()) {
        query.filter_repo_id(settings.to_repo_ids, sack::QueryCmp::GLOB);
        if (query.empty()) {
            transaction.p_impl->add_resolve_log(
                action,
                GoalProblem::NOT_FOUND_IN_REPOSITORIES,
                settings,
                libdnf5::transaction::TransactionItemType::PACKAGE,
                spec,
                {},
                libdnf5::Logger::Level::WARNING);
            return skip_unavailable ? GoalProblem::NO_PROBLEM : GoalProblem::NOT_FOUND_IN_REPOSITORIES;
        }
        query |= installed;
    }

    // Apply advisory filters
    if (settings.advisory_filter.has_value()) {
        filter_candidates_for_advisory_upgrade(base, query, settings.advisory_filter.value(), obsoletes);
    }

    if (minimal) {
        query.filter_earliest_evr();
    }

    switch (action) {
        case GoalAction::UPGRADE_MINIMAL:
        case GoalAction::UPGRADE:
            query.filter_available();
            // Given that we use libsolv's targeted transactions, we need to ensure that the transaction contains both
            // the new targeted version and also the current installed version (for the upgraded package). This is
            // because if it only contained the new version, libsolv would decide to reinstall the package even if it
            // had just a different buildtime or vendor but the same version
            // (https://github.com/openSUSE/libsolv/issues/287)
            //   - Make sure that query contains both the new and installed versions (i.e. add installed versions)
            //   - However we need to add installed versions of just the packages that are being upgraded. We don't want
            //     to add all installed packages because it could increase the number of solutions for the transaction
            //     (especially with --no-best) and since libsolv prefers the smallest possible upgrade it could result
            //     in no upgrade even if there is one available. This is a problem in general but its critical with
            //     --security transactions (https://bugzilla.redhat.com/show_bug.cgi?id=2097757)
            all_installed.filter_name(query);
            //   - We want to add only the latest versions of installed packages, this is specifically for installonly
            //     packages. Otherwise if for example kernel-1 and kernel-3 were installed and present in the
            //     transaction libsolv could decide to install kernel-2 because it is an upgrade for kernel-1 even
            //     though we don't want it because there already is a newer version present.
            all_installed.filter_latest_evr();
            query |= all_installed;
            solv_map_to_id_queue(tmp_queue, *query.p_impl);
            rpm_goal.add_upgrade(tmp_queue, best, clean_requirements_on_remove);
            break;
        case GoalAction::DISTRO_SYNC:
            solv_map_to_id_queue(tmp_queue, *query.p_impl);
            rpm_goal.add_distro_sync(tmp_queue, skip_broken, best, clean_requirements_on_remove);
            break;
        case GoalAction::DOWNGRADE: {
            query.filter_available();
            query.filter_downgrades();
            auto & pool = get_rpm_pool(base);
            std::vector<Solvable *> tmp_solvables;
            for (auto pkg_id : *query.p_impl) {
                tmp_solvables.push_back(pool.id2solvable(pkg_id));
            }
            std::sort(tmp_solvables.begin(), tmp_solvables.end(), nevra_solvable_cmp_key);
            std::map<Id, std::vector<Id>> name_arches;
            // Make for each name arch only one downgrade job
            for (auto installed_id : *relevant_installed_na.p_impl) {
                Solvable * solvable = pool.id2solvable(installed_id);
                auto & arches = name_arches[solvable->name];
                bool unique = true;
                for (Id arch : arches) {
                    if (arch == solvable->arch) {
                        unique = false;
                        break;
                    }
                }
                if (unique) {
                    arches.push_back(solvable->arch);
                    tmp_queue.clear();
                    auto low = std::lower_bound(
                        tmp_solvables.begin(), tmp_solvables.end(), solvable, name_arch_compare_lower_solvable);
                    while (low != tmp_solvables.end() && (*low)->name == solvable->name &&
                           (*low)->arch == solvable->arch) {
                        tmp_queue.push_back(pool.solvable2id(*low));
                        ++low;
                    }
                    if (tmp_queue.empty()) {
                        std::string name_arch(pool.get_name(installed_id));
                        name_arch.append(".");
                        name_arch.append(pool.get_arch(installed_id));
                        transaction.p_impl->add_resolve_log(
                            action,
                            GoalProblem::INSTALLED_LOWEST_VERSION,
                            settings,
                            libdnf5::transaction::TransactionItemType::PACKAGE,
                            spec,
                            {name_arch},
                            libdnf5::Logger::Level::WARNING);
                    } else {
                        rpm_goal.add_install(tmp_queue, skip_broken, best, clean_requirements_on_remove);
                    }
                }
            }
        } break;
        default:
            throw std::invalid_argument("Unsupported action");
    }
    return GoalProblem::NO_PROBLEM;
}

void Goal::Impl::install_group_package(base::Transaction & transaction, libdnf5::comps::Package pkg) {
    auto pkg_settings = GoalJobSettings();
    pkg_settings.with_provides = false;
    pkg_settings.with_filenames = false;
    pkg_settings.with_binaries = false;
    pkg_settings.nevra_forms.push_back(rpm::Nevra::Form::NAME);

    // TODO(mblaha): apply pkg.basearchonly when available in comps
    auto pkg_name = pkg.get_name();
    auto pkg_condition = pkg.get_condition();
    if (pkg_condition.empty()) {
        auto [pkg_problem, pkg_queue] =
            // TODO(mblaha): add_install_to_goal needs group spec for better problems reporting
            add_install_to_goal(transaction, GoalAction::INSTALL_BY_COMPS, pkg_name, pkg_settings);
        rpm_goal.add_transaction_group_installed(pkg_queue);
    } else {
        // check whether condition can even be met
        rpm::PackageQuery condition_query(base);
        condition_query.filter_name(std::vector<std::string>{pkg_condition});
        if (!condition_query.empty()) {
            // remember names to identify GROUP reason of conditional packages
            rpm::PackageQuery query(base);
            query.filter_name(std::vector<std::string>{pkg_name});
            // TODO(mblaha): log absence of pkg in case the query is empty
            if (!query.empty()) {
                add_provide_install_to_goal(fmt::format("({} if {})", pkg_name, pkg_condition), pkg_settings);
                rpm_goal.add_transaction_group_installed(*query.p_impl);
            }
        }
    }
}

void Goal::Impl::remove_group_packages(const rpm::PackageSet & remove_candidates) {
    // all installed packages, that are not candidates for removal
    rpm::PackageQuery dependent_base(base);
    dependent_base.filter_installed();
    {
        // create auxiliary goal to resolve all unused dependencies that are going to be
        // removed together with removal candidates
        Goal goal_tmp(base);
        goal_tmp.add_rpm_remove(remove_candidates);
        for (const auto & tspkg : goal_tmp.resolve().get_transaction_packages()) {
            if (transaction_item_action_is_outbound(tspkg.get_action())) {
                dependent_base.remove(tspkg.get_package());
            }
        }
    }

    // The second step of packages removal - filter out packages that are
    // dependencies of a package that is not also being removed.
    libdnf5::solv::IdQueue packages_to_remove_ids;
    for (const auto & pkg : remove_candidates) {
        // if the package is required by another installed package, it is
        // not removed, but it's reason is changed to DEPENDENCY
        rpm::PackageQuery dependent(dependent_base);
        dependent.filter_requires(pkg.get_provides());
        if (dependent.size() > 0) {
            rpm_goal.add_reason_change(pkg, transaction::TransactionItemReason::DEPENDENCY, std::nullopt);
        } else {
            packages_to_remove_ids.push_back(pkg.get_id().id);
        }
    }

    auto & cfg_main = base->get_config();
    rpm_goal.add_remove(packages_to_remove_ids, cfg_main.get_clean_requirements_on_remove_option().get_value());
}

void Goal::Impl::add_group_install_to_goal(
    base::Transaction & transaction,
    const transaction::TransactionItemReason reason,
    comps::GroupQuery group_query,
    GoalJobSettings & settings) {
    auto & cfg_main = base->get_config();
    auto allowed_package_types = settings.resolve_group_package_types(cfg_main);
    for (auto group : group_query) {
        rpm_goal.add_group(group, transaction::TransactionItemAction::INSTALL, reason, allowed_package_types);
        if (settings.group_no_packages) {
            continue;
        }
        std::vector<libdnf5::comps::Package> packages;
        // TODO(mblaha): filter packages by p.arch attribute when supported by comps
        for (const auto & p : group.get_packages()) {
            if (any(allowed_package_types & p.get_type())) {
                packages.emplace_back(std::move(p));
            }
        }
        for (const auto & pkg : packages) {
            install_group_package(transaction, pkg);
        }
    }
}

void Goal::Impl::add_group_remove_to_goal(
    std::vector<std::tuple<std::string, transaction::TransactionItemReason, comps::GroupQuery, GoalJobSettings>> &
        groups_to_remove) {
    if (groups_to_remove.empty()) {
        return;
    }

    // get list of group ids being removed in this transaction
    std::set<std::string> removed_groups_ids;
    for (auto & [spec, reason, group_query, settings] : groups_to_remove) {
        for (const auto & group : group_query) {
            removed_groups_ids.emplace(group.get_groupid());
        }
    }
    rpm::PackageQuery query_installed(base);
    query_installed.filter_installed();
    auto & system_state = base->p_impl->get_system_state();
    // packages that are candidates for removal
    rpm::PackageSet remove_candidates(base);
    for (auto & [spec, reason, group_query, settings] : groups_to_remove) {
        for (const auto & group : group_query) {
            rpm_goal.add_group(group, transaction::TransactionItemAction::REMOVE, reason, {});
            if (settings.group_no_packages) {
                continue;
            }
            // get all packages installed by the group
            rpm::PackageQuery group_packages(query_installed);
            group_packages.filter_name(system_state.get_group_state(group.get_groupid()).packages);
            // Remove packages installed by the group.
            // First collect packages that are not part of any other
            // installed group and are not user-installed.
            for (const auto & pkg : group_packages) {
                // is the package part of another group which is not being removed?
                auto pkg_groups = system_state.get_package_groups(pkg.get_name());
                // remove from the list all groups being removed in this transaction
                for (const auto & id : removed_groups_ids) {
                    pkg_groups.erase(id);
                }
                if (pkg_groups.size() > 0) {
                    continue;
                }

                // was the package user-installed?
                if (pkg.get_reason() > transaction::TransactionItemReason::GROUP) {
                    continue;
                }

                remove_candidates.add(pkg);
            }
        }
    }
    if (remove_candidates.empty()) {
        return;
    }

    remove_group_packages(remove_candidates);
}

void Goal::Impl::add_group_upgrade_to_goal(
    base::Transaction & transaction, comps::GroupQuery group_query, GoalJobSettings & settings) {
    auto & system_state = base->p_impl->get_system_state();
    auto & cfg_main = base->get_config();
    auto allowed_package_types = settings.resolve_group_package_types(cfg_main);

    comps::GroupQuery available_groups(base);
    available_groups.filter_installed(false);

    rpm::PackageQuery query_installed(base);
    query_installed.filter_installed();
    rpm::PackageSet remove_candidates(base);

    for (auto installed_group : group_query) {
        auto group_id = installed_group.get_groupid();
        // find available group of the same id
        comps::GroupQuery available_group_query(available_groups);
        available_group_query.filter_groupid(group_id);
        if (available_group_query.empty()) {
            // group is not available any more
            transaction.p_impl->add_resolve_log(
                GoalAction::UPGRADE,
                GoalProblem::NOT_AVAILABLE,
                settings,
                libdnf5::transaction::TransactionItemType::GROUP,
                group_id,
                {},
                libdnf5::Logger::Level::WARNING);
            continue;
        }
        auto available_group = available_group_query.get();
        auto state_group = system_state.get_group_state(group_id);

        // set of package names that are part of the installed version of the group
        std::set<std::string> old_set{};
        for (const auto & pkg : installed_group.get_packages()) {
            old_set.emplace(pkg.get_name());
        }
        // set of package names that are part of the available version of the group
        std::set<std::string> new_set{};
        for (const auto & pkg : available_group.get_packages()) {
            new_set.emplace(pkg.get_name());
        }

        // install packages newly added to the group
        for (const auto & pkg : available_group.get_packages_of_type(state_group.package_types)) {
            if (!old_set.contains(pkg.get_name())) {
                install_group_package(transaction, pkg);
            }
        }

        auto pkg_settings = GoalJobSettings();
        pkg_settings.with_provides = false;
        pkg_settings.with_filenames = false;
        pkg_settings.with_binaries = false;
        for (const auto & pkg_name : state_group.packages) {
            if (new_set.contains(pkg_name)) {
                // upgrade all packages installed with the group
                pkg_settings.nevra_forms.push_back(rpm::Nevra::Form::NAME);
                add_up_down_distrosync_to_goal(transaction, GoalAction::UPGRADE, pkg_name, pkg_settings);
            } else {
                // remove those packages that are not part of the group any more
                // and are not user-installed
                rpm::PackageQuery query(query_installed);
                auto nevra_pair = query.resolve_pkg_spec(pkg_name, pkg_settings, false);
                if (nevra_pair.first) {
                    for (const auto & pkg : query) {
                        if (pkg.get_reason() <= transaction::TransactionItemReason::GROUP) {
                            remove_candidates.add(pkg);
                        }
                    }
                }
            }
        }
        // upgrade the group itself
        rpm_goal.add_group(
            available_group,
            transaction::TransactionItemAction::UPGRADE,
            installed_group.get_reason(),
            allowed_package_types);
    }
    if (!remove_candidates.empty()) {
        remove_group_packages(remove_candidates);
    }
}

void Goal::Impl::add_environment_install_to_goal(
    base::Transaction & transaction, comps::EnvironmentQuery environment_query, GoalJobSettings & settings) {
    auto & cfg_main = base->get_config();
    bool with_optional = any(settings.resolve_group_package_types(cfg_main) & libdnf5::comps::PackageType::OPTIONAL);
    auto group_settings = libdnf5::GoalJobSettings(settings);
    group_settings.group_search_environments = false;
    group_settings.group_search_groups = true;
    std::vector<GroupSpec> env_group_specs;
    for (auto environment : environment_query) {
        rpm_goal.add_environment(environment, transaction::TransactionItemAction::INSTALL, with_optional);
        for (const auto & grp_id : environment.get_groups()) {
            env_group_specs.emplace_back(
                GoalAction::INSTALL_BY_COMPS, transaction::TransactionItemReason::DEPENDENCY, grp_id, group_settings);
        }
        if (with_optional) {
            for (const auto & grp_id : environment.get_optional_groups()) {
                env_group_specs.emplace_back(
                    GoalAction::INSTALL_BY_COMPS,
                    transaction::TransactionItemReason::DEPENDENCY,
                    grp_id,
                    group_settings);
            }
        }
    }
    resolve_group_specs(env_group_specs, transaction);
}

void Goal::Impl::add_environment_remove_to_goal(
    base::Transaction & transaction,
    std::vector<std::tuple<std::string, comps::EnvironmentQuery, GoalJobSettings>> & environments_to_remove) {
    if (environments_to_remove.empty()) {
        return;
    }

    // get list of environment ids being removed in this transaction
    std::set<std::string> removed_environments_ids;
    for (auto & [spec, environment_query, settings] : environments_to_remove) {
        for (const auto & environment : environment_query) {
            removed_environments_ids.emplace(environment.get_environmentid());
        }
    }
    comps::GroupQuery query_installed(base);
    query_installed.filter_installed(true);
    auto & system_state = base->p_impl->get_system_state();
    // groups that are candidates for removal
    std::vector<GroupSpec> remove_group_specs;
    auto group_settings = libdnf5::GoalJobSettings();
    group_settings.group_search_environments = false;
    group_settings.group_search_groups = true;
    for (auto & [spec, environment_query, settings] : environments_to_remove) {
        for (const auto & environment : environment_query) {
            rpm_goal.add_environment(environment, transaction::TransactionItemAction::REMOVE, {});
            // get all groups installed by the environment
            comps::GroupQuery environment_groups(query_installed);
            environment_groups.filter_groupid(
                system_state.get_environment_state(environment.get_environmentid()).groups);
            // Remove groups installed by the environment in case they are installed
            // as dependencies and are not part of another installed environment.
            for (const auto & grp : environment_groups) {
                // is the group part of another environment which is not being removed?
                auto grp_environments = system_state.get_group_environments(grp.get_groupid());
                // remove from the list all environments being removed in this transaction
                for (const auto & id : removed_environments_ids) {
                    grp_environments.erase(id);
                }
                if (grp_environments.size() > 0) {
                    continue;
                }

                // was the group user-installed?
                if (grp.get_reason() > transaction::TransactionItemReason::GROUP) {
                    continue;
                }

                remove_group_specs.emplace_back(
                    GoalAction::REMOVE,
                    transaction::TransactionItemReason::DEPENDENCY,
                    grp.get_groupid(),
                    group_settings);
            }
        }
    }
    resolve_group_specs(remove_group_specs, transaction);
}

void Goal::Impl::add_environment_upgrade_to_goal(
    base::Transaction & transaction, comps::EnvironmentQuery environment_query, GoalJobSettings & settings) {
    auto & system_state = base->p_impl->get_system_state();

    comps::EnvironmentQuery available_environments(base);
    available_environments.filter_installed(false);

    comps::GroupQuery query_installed(base);
    query_installed.filter_installed(true);
    std::vector<GroupSpec> remove_group_specs;

    std::vector<GroupSpec> env_group_specs;
    auto group_settings = libdnf5::GoalJobSettings(settings);
    group_settings.group_search_environments = false;
    group_settings.group_search_groups = true;

    for (auto installed_environment : environment_query) {
        auto environment_id = installed_environment.get_environmentid();
        // find available environment of the same id
        comps::EnvironmentQuery available_environment_query(available_environments);
        available_environment_query.filter_environmentid(environment_id);
        if (available_environment_query.empty()) {
            // environment is not available any more
            transaction.p_impl->add_resolve_log(
                GoalAction::UPGRADE,
                GoalProblem::NOT_AVAILABLE,
                settings,
                libdnf5::transaction::TransactionItemType::ENVIRONMENT,
                environment_id,
                {},
                libdnf5::Logger::Level::WARNING);
            continue;
        }
        auto available_environment = available_environment_query.get();

        // upgrade the environment itself
        rpm_goal.add_environment(available_environment, transaction::TransactionItemAction::UPGRADE, {});

        // group names that are part of the installed version of the environment
        auto old_groups = installed_environment.get_groups();

        // group names that are part of the new version of the environment
        auto available_groups = available_environment.get_groups();

        for (const auto & grp : available_groups) {
            if (std::find(old_groups.begin(), old_groups.end(), grp) != old_groups.end()) {
                // the group was already part of environment definition when it was installed.
                // upgrade the group if is installed
                try {
                    auto group_state = system_state.get_group_state(grp);
                    env_group_specs.emplace_back(
                        GoalAction::UPGRADE, transaction::TransactionItemReason::DEPENDENCY, grp, group_settings);
                } catch (const system::StateNotFoundError &) {
                    continue;
                }
            } else {
                // newly added group to environment definition, install it.
                env_group_specs.emplace_back(
                    GoalAction::INSTALL_BY_COMPS, transaction::TransactionItemReason::DEPENDENCY, grp, group_settings);
            }
        }

        // upgrade also installed optional groups
        auto old_optionals = installed_environment.get_optional_groups();
        old_groups.insert(old_groups.end(), old_optionals.begin(), old_optionals.end());
        for (const auto & grp : available_environment.get_optional_groups()) {
            available_groups.emplace_back(grp);
            if (std::find(old_groups.begin(), old_groups.end(), grp) != old_groups.end()) {
                try {
                    auto group_state = system_state.get_group_state(grp);
                    env_group_specs.emplace_back(
                        GoalAction::UPGRADE, transaction::TransactionItemReason::DEPENDENCY, grp, group_settings);
                } catch (const system::StateNotFoundError &) {
                    continue;
                }
            }
        }

        // remove non-userinstalled groups that are not part of environment any more
        for (const auto & grp : old_groups) {
            if (std::find(available_groups.begin(), available_groups.end(), grp) == available_groups.end()) {
                try {
                    auto group_state = system_state.get_group_state(grp);
                    if (!group_state.userinstalled) {
                        auto grp_environments = system_state.get_group_environments(grp);
                        grp_environments.erase(environment_id);
                        if (grp_environments.empty()) {
                            env_group_specs.emplace_back(
                                GoalAction::REMOVE,
                                transaction::TransactionItemReason::DEPENDENCY,
                                grp,
                                group_settings);
                        }
                    }
                } catch (const system::StateNotFoundError &) {
                    continue;
                }
            }
        }
    }

    resolve_group_specs(env_group_specs, transaction);
}

GoalProblem Goal::Impl::add_reason_change_to_goal(
    base::Transaction & transaction,
    const std::string & spec,
    const transaction::TransactionItemReason reason,
    const std::optional<std::string> & group_id,
    GoalJobSettings & settings) {
    auto & cfg_main = base->get_config();
    bool skip_unavailable = settings.resolve_skip_unavailable(cfg_main);
    auto log_level = skip_unavailable ? libdnf5::Logger::Level::WARNING : libdnf5::Logger::Level::ERROR;
    rpm::PackageQuery query(base);
    query.filter_installed();
    auto nevra_pair = query.resolve_pkg_spec(spec, settings, false);
    if (!nevra_pair.first) {
        auto problem = transaction.p_impl->report_not_found(GoalAction::REASON_CHANGE, spec, settings, log_level);
        if (skip_unavailable) {
            return GoalProblem::NO_PROBLEM;
        } else {
            return problem;
        }
    }
    for (const auto & pkg : query) {
        if (pkg.get_reason() == reason) {
            // pkg is already installed with correct reason
            transaction.p_impl->add_resolve_log(
                GoalAction::REASON_CHANGE,
                GoalProblem::ALREADY_INSTALLED,
                settings,
                libdnf5::transaction::TransactionItemType::PACKAGE,
                pkg.get_nevra(),
                {libdnf5::transaction::transaction_item_reason_to_string(reason)},
                libdnf5::Logger::Level::WARNING);
            continue;
        }
        rpm_goal.add_reason_change(pkg, reason, group_id);
    }
    return GoalProblem::NO_PROBLEM;
}

void Goal::Impl::add_paths_to_goal() {
    if (rpm_filepaths.empty()) {
        return;
    }

    // fill the command line repo with paths to rpm files
    std::vector<std::string> paths;
    for (const auto & [action, path, settings] : rpm_filepaths) {
        paths.emplace_back(path);
    }
    auto cmdline_packages = base->get_repo_sack()->add_cmdline_packages(paths);

    // add newly created packages to the goal
    for (const auto & [action, path, settings] : rpm_filepaths) {
        auto pkg = cmdline_packages.find(path);
        if (pkg != cmdline_packages.end()) {
            add_rpm_ids(action, pkg->second, settings);
        }
    }

    // clear rpm_filepaths so that they do not get inserted into command line
    // repo again in case the goal is resolved multiple times.
    rpm_filepaths.clear();
}

void Goal::Impl::set_exclude_from_weak(const std::vector<std::string> & exclude_from_weak) {
    for (const auto & exclude_weak : exclude_from_weak) {
        rpm::PackageQuery weak_query(base, rpm::PackageQuery::ExcludeFlags::APPLY_EXCLUDES);
        libdnf5::ResolveSpecSettings settings{
            .with_nevra = true, .with_provides = false, .with_filenames = true, .with_binaries = false};
        weak_query.resolve_pkg_spec(exclude_weak, settings, false);
        weak_query.filter_available();
        rpm_goal.add_exclude_from_weak(*weak_query.p_impl);
    }
}

void Goal::Impl::autodetect_unsatisfied_installed_weak_dependencies() {
    rpm::PackageQuery installed_query(base, rpm::PackageQuery::ExcludeFlags::IGNORE_EXCLUDES);
    installed_query.filter_installed();
    if (installed_query.empty()) {
        return;
    }
    rpm::PackageQuery base_query(base, rpm::PackageQuery::ExcludeFlags::APPLY_EXCLUDES);
    rpm::ReldepList reldep_list(base);

    std::vector<std::string> installed_names;
    installed_names.reserve(installed_query.size());

    // Investigate uninstalled recommends of installed packages
    for (const auto & pkg : installed_query) {
        installed_names.push_back(pkg.get_name());
        for (const auto & recommend : pkg.get_recommends()) {
            if (libdnf5::rpm::Reldep::is_rich_dependency(recommend.to_string())) {
                // Rich dependencies are skipped because they are too complicated to provide correct result
                continue;
            };
            rpm::PackageQuery query(base_query);

            //  There can be installed provider in a different version or upgraded package can recommend a different
            //  version therefore ignore the version and to search only using reldep name
            if (auto version = recommend.get_version(); version && strlen(version) > 0) {
                auto & pool = get_rpm_pool(base);
                Id id = pool.str2id(recommend.get_name(), 0);
                reldep_list.add(rpm::ReldepId(id));
            } else {
                reldep_list.add(recommend);
            };
            query.filter_provides(reldep_list);
            reldep_list.clear();
            // No providers of recommend => continue
            if (query.empty()) {
                continue;
            }
            rpm::PackageQuery test_installed(query);
            test_installed.filter_installed();
            // when there is not installed any provider of recommend, exclude it
            if (test_installed.empty()) {
                rpm_goal.add_exclude_from_weak(*query.p_impl);
            }
        }
    }

    // Investigate supplements of only available packages with a different name to installed packages
    // We can use base_query, because it is not useful anymore
    base_query.filter_name(installed_names, sack::QueryCmp::NEQ);
    // We have to remove all installed packages from testing set
    base_query -= installed_query;
    rpm::PackageSet exclude_supplements(base);
    for (const auto & pkg : base_query) {
        auto supplements = pkg.get_supplements();
        if (supplements.empty()) {
            continue;
        }
        for (const auto & supplement : supplements) {
            if (libdnf5::rpm::Reldep::is_rich_dependency(supplement.to_string())) {
                // Rich dependencies are skipped because they are too complicated to provide correct result
                continue;
            };
            reldep_list.add(supplement);
        }
        if (reldep_list.empty()) {
            continue;
        }
        rpm::PackageQuery query(installed_query);
        query.filter_provides(reldep_list);
        reldep_list.clear();
        if (!query.empty()) {
            exclude_supplements.add(pkg);
        }
    }
    if (!exclude_supplements.empty()) {
        rpm_goal.add_exclude_from_weak(*exclude_supplements.p_impl);
    }
}

void Goal::set_allow_erasing(bool value) {
    p_impl->allow_erasing = value;
}

bool Goal::get_allow_erasing() const {
    return p_impl->allow_erasing;
}

base::Transaction Goal::resolve() {
    libdnf_user_assert(p_impl->base->is_initialized(), "Base instance was not fully initialized by Base::setup()");

    // Enable modules
    module::ModuleSack & module_sack = p_impl->base->module_sack;
    for (auto module_enable_spec : p_impl->module_enable_specs) {
        module_sack.p_impl->enable(module_enable_spec);
    }

    // Resolve modules
    auto module_error = module_sack.resolve_active_module_items().second;
    if (module_error != libdnf5::module::ModuleSack::ModuleErrorType::NO_ERROR) {
        throw module::ModuleResolveError(M_("Failed to resolve modules."));
    }
    module_sack.p_impl->enable_dependent_modules();

    p_impl->rpm_goal = rpm::solv::GoalPrivate(p_impl->base);

    p_impl->add_paths_to_goal();

    auto sack = p_impl->base->get_rpm_package_sack();
    base::Transaction transaction(p_impl->base);
    auto ret = GoalProblem::NO_PROBLEM;

    sack->p_impl->recompute_considered_in_pool();
    sack->p_impl->make_provides_ready();
    // TODO(jmracek) Apply modules first
    // TODO(jmracek) Apply comps second or later
    // TODO(jmracek) Reset rpm_goal, setup rpm-goal flags according to conf, (allow downgrade), obsoletes, vendor, ...
    ret |= p_impl->add_specs_to_goal(transaction);
    p_impl->add_rpms_to_goal(transaction);

    // Resolve group specs to group/environment queries first for two reasons:
    // 1. group spec can also contain an environmental groups
    // 2. group removal needs a list of all groups being removed to correctly remove packages
    ret |= p_impl->resolve_group_specs(p_impl->group_specs, transaction);

    // Handle environments befor groups because they will add/remove groups
    p_impl->add_resolved_environment_specs_to_goal(transaction);

    // Then handle groups
    p_impl->add_resolved_group_specs_to_goal(transaction);

    ret |= p_impl->add_reason_change_specs_to_goal(transaction);

    auto & cfg_main = p_impl->base->get_config();
    // Set goal flags
    p_impl->rpm_goal.set_allow_vendor_change(cfg_main.get_allow_vendor_change_option().get_value());
    p_impl->rpm_goal.set_allow_erasing(p_impl->allow_erasing);
    p_impl->rpm_goal.set_install_weak_deps(cfg_main.get_install_weak_deps_option().get_value());
    p_impl->rpm_goal.set_allow_downgrade(cfg_main.get_allow_downgrade_option().get_value());

    if (cfg_main.get_protect_running_kernel_option().get_value()) {
        p_impl->rpm_goal.set_protected_running_kernel(sack->p_impl->get_running_kernel_id());
    }

    // Set user-installed packages (installed packages with reason USER or GROUP)
    // proceed only if the transaction could result in removal of unused dependencies
    if (p_impl->rpm_goal.is_clean_deps_present()) {
        libdnf5::solv::IdQueue user_installed_packages;
        rpm::PackageQuery installed_query(p_impl->base, rpm::PackageQuery::ExcludeFlags::IGNORE_EXCLUDES);
        installed_query.filter_installed();
        for (const auto & pkg : installed_query) {
            if (pkg.get_reason() > transaction::TransactionItemReason::DEPENDENCY) {
                user_installed_packages.push_back(pkg.get_id().id);
            }
        }
        p_impl->rpm_goal.set_user_installed_packages(std::move(user_installed_packages));
    }

    // Add protected packages
    {
        auto & protected_packages = cfg_main.get_protected_packages_option().get_value();
        rpm::PackageQuery protected_query(p_impl->base, rpm::PackageQuery::ExcludeFlags::IGNORE_EXCLUDES);
        protected_query.filter_name(protected_packages);
        p_impl->rpm_goal.add_protected_packages(*protected_query.p_impl);
    }

    // Set installonly packages
    {
        auto & installonly_packages = cfg_main.get_installonlypkgs_option().get_value();
        p_impl->rpm_goal.set_installonly(installonly_packages);
        p_impl->rpm_goal.set_installonly_limit(cfg_main.get_installonly_limit_option().get_value());
    }

    // Set exclude weak dependencies from configuration
    {
        p_impl->set_exclude_from_weak(cfg_main.get_exclude_from_weak_option().get_value());
        if (cfg_main.get_exclude_from_weak_autodetect_option().get_value()) {
            p_impl->autodetect_unsatisfied_installed_weak_dependencies();
        }
    }

    ret |= p_impl->rpm_goal.resolve();

    // Write debug solver data
    if (cfg_main.get_debug_solver_option().get_value()) {
        auto debug_dir = std::filesystem::path(cfg_main.get_debugdir_option().get_value()) / "packages";
        auto abs_debug_dir = std::filesystem::absolute(debug_dir);

        // Ensures the presence of the directory.
        std::filesystem::create_directories(abs_debug_dir);

        p_impl->rpm_goal.write_debugdata(abs_debug_dir);

        transaction.p_impl->add_resolve_log(
            GoalAction::RESOLVE,
            GoalProblem::WRITE_DEBUG,
            {},
            libdnf5::transaction::TransactionItemType::PACKAGE,
            "",
            {abs_debug_dir},
            libdnf5::Logger::Level::WARNING);
    }

    transaction.p_impl->set_transaction(p_impl->rpm_goal, module_sack, ret);

    return transaction;
}

void Goal::reset() {
    p_impl->module_enable_specs.clear();
    p_impl->rpm_specs.clear();
    p_impl->rpm_ids.clear();
    p_impl->group_specs.clear();
    p_impl->rpm_filepaths.clear();
    p_impl->resolved_group_specs.clear();
    p_impl->resolved_environment_specs.clear();
    p_impl->rpm_goal = rpm::solv::GoalPrivate(p_impl->base);
}

BaseWeakPtr Goal::get_base() const {
    return p_impl->base->get_weak_ptr();
}

}  // namespace libdnf5
/*
Copyright Contributors to the libdnf project.

This file is part of libdnf: https://github.com/rpm-software-management/libdnf/

Libdnf is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

Libdnf is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with libdnf.  If not, see <https://www.gnu.org/licenses/>.
*/


#ifndef MICRODNF_COMMANDS_CHANGELOG_HPP
#define MICRODNF_COMMANDS_CHANGELOG_HPP


#include <libdnf-cli/session.hpp>
#include <libdnf/conf/option_bool.hpp>
#include <libdnf/conf/option_number.hpp>

#include <memory>
#include <vector>


namespace microdnf {


class ChangelogCommand : public libdnf::cli::session::Command {
public:
    explicit ChangelogCommand(Command & parent);
    void run() override;

private:
    libdnf::OptionNumber<std::int32_t> * count_option{nullptr};
    libdnf::OptionNumber<std::int64_t> * since_option{nullptr};
    libdnf::OptionBool * upgrades_option{nullptr};
    std::vector<std::unique_ptr<libdnf::Option>> * pkgs_spec_to_show_options{nullptr};
};


}  // namespace microdnf


#endif  // MICRODNF_COMMANDS_CHANGELOG_HPP
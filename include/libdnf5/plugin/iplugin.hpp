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

#ifndef LIBDNF5_PLUGIN_IPLUGIN_HPP
#define LIBDNF5_PLUGIN_IPLUGIN_HPP

#include "libdnf5/version.hpp"

#include <cstdint>

namespace libdnf5 {

class Base;
struct ConfigParser;

namespace base {
class Transaction;
}

}  // namespace libdnf5

namespace libdnf5::plugin {

/// Plugin version
struct Version {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t micro;
};

class IPlugin {
public:
    IPlugin(Base & base) : base(&base) {}
    virtual ~IPlugin() = default;

    IPlugin(const IPlugin &) = delete;
    IPlugin(IPlugin &&) = delete;
    IPlugin & operator=(const IPlugin &) = delete;
    IPlugin & operator=(IPlugin &&) = delete;

    /// Returns the version of the API supported by the plugin. It can be called at any time.
    virtual PluginAPIVersion get_api_version() const noexcept = 0;

    /// Returns the name of the plugin. It can be called at any time.
    virtual const char * get_name() const noexcept = 0;

    /// Gets the plugin version. It can be called at any time.
    virtual Version get_version() const noexcept = 0;

    /// @return A nullptr terminated array of attributes supported by the plugin.
    virtual const char * const * get_attributes() const noexcept = 0;

    /// Gets the value of the attribute from the plugin. Returns nullptr if the attribute does not exist.
    /// It can be called at any time.
    virtual const char * get_attribute(const char * name) const noexcept = 0;

    /// The plugin can load additional plugins. E.g. C++ plugin for loading Python plugins.
    /// Called before init.
    virtual void load_plugins() {}

    /// Plugin initialization.
    /// Called before hooks.
    virtual void init() {}

    /// The pre_base_setup hook.
    /// It is called at the beginning of the `Base::setup` method (after the `init` hook).
    virtual void pre_base_setup() {}

    /// The post_base_setup hook.
    /// It is called at the end of the `Base::setup` method.
    virtual void post_base_setup() {}

    /// The pre_transaction hook.
    /// It is called just before the actual transaction starts.
    virtual void pre_transaction(const libdnf5::base::Transaction &) {}

    /// The post_transaction hook.
    /// It is called after transactions.
    virtual void post_transaction(const libdnf5::base::Transaction &) {}

    /// Finish the plugin and release all resources obtained by the init method and in hooks.
    virtual void finish() noexcept {}

    Base & get_base() noexcept { return *base; }

private:
    Base * base;
};

}  // namespace libdnf5::plugin


extern "C" {

/// Returns the version of the API supported by the plugin.
/// Same result as IPlugin::get_api_version(), but can be called without creating an IPlugin instance.
libdnf5::PluginAPIVersion libdnf_plugin_get_api_version(void);

/// Returns the name of the plugin. It can be called at any time.
/// Same result as IPlugin::get_name(), but can be called without creating an IPlugin instance.
const char * libdnf_plugin_get_name(void);

/// Returns the version of the plugin. It can be called at any time.
/// Same result as IPlugin::get_version(), but can be called without creating an IPlugin instance.
libdnf5::plugin::Version libdnf_plugin_get_version(void);

/// Creates a new plugin instance. Passes the API version to the plugin.
libdnf5::plugin::IPlugin * libdnf_plugin_new_instance(
    libdnf5::LibraryVersion library_version, libdnf5::Base & base, libdnf5::ConfigParser & parser);

/// Deletes plugin instance.
void libdnf_plugin_delete_instance(libdnf5::plugin::IPlugin * plugin_instance);
}

#endif

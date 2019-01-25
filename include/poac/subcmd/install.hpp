#ifndef POAC_SUBCMD_INSTALL_HPP
#define POAC_SUBCMD_INSTALL_HPP

#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <fstream>
#include <map>

#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <yaml-cpp/yaml.h>

#include "../io.hpp"
#include "../core/exception.hpp"
#include "../core/resolver.hpp"
#include "../util.hpp"


// Version 1 will allow $ poac install <Nothing> only.
//   Parse poac.yml
// Version 2 will also allow $ poac install [<pkg-names>].
//   Parse arguments.
// TODO: --source (source file only (not pre-built))
namespace poac::subcmd {
    namespace _install {
        // Copy package to ./deps
        bool copy_to_current(const std::string& from, const std::string& to) {
            namespace path = io::file::path;
            const auto from_path = path::poac_cache_dir / from;
            const auto to_path = path::current_deps_dir / to;
            return path::recursive_copy(from_path, to_path);
        }

        void echo_install_status(const bool res, const std::string& n, const std::string& v, const std::string& s) {
            const std::string status = n + " " + v + " (from: " + s + ")";
            io::cli::echo(res ? io::cli::to_fetch_failed(status) : io::cli::to_fetched(status));
        }


        void stream_deps(YAML::Emitter& out, const core::resolver::Activated& deps) {
            out << YAML::Key << "dependencies";
            out << YAML::Value << YAML::BeginMap;

            for (const auto& dep : deps) {
                out << YAML::Key << dep.name;
                out << YAML::Value << YAML::BeginMap;

                out << YAML::Key << "version";
                out << YAML::Value << dep.version;

                out << YAML::Key << "source";
                out << YAML::Value << dep.source;

                if (!dep.deps.empty()) {
                    stream_deps(out, dep.deps);
                }
                out << YAML::EndMap;
            }
            out << YAML::EndMap;
        }

        void create_lock_file(const std::string& timestamp, const core::resolver::Activated& activated_deps) {
            // Create a poac.lock
            if (std::ofstream ofs("poac.lock"); ofs) {
                ofs << "# Please do not edit this file.\n";
                YAML::Emitter out;
                out << YAML::BeginMap;

                out << YAML::Key << "timestamp";
                out << YAML::Value << timestamp;

                stream_deps(out, activated_deps);
                ofs << out.c_str() << '\n';
            }
        }


        void fetch_packages(
                const core::resolver::Backtracked& deps,
                const bool quite,
                const bool verbose)
        {
            namespace except = core::exception;
            namespace naming = core::naming;
            namespace path = io::file::path;
            namespace tb = io::file::tarball;
            namespace resolver = core::resolver;
            namespace fs = boost::filesystem;

            int exists_count = 0;
            for (const auto& [name, dep] : deps) {
                const auto cache_name = naming::to_cache(dep.source, name, dep.version);
                const auto current_name = naming::to_current(dep.source, name, dep.version);
                const bool is_cached = resolver::cache::resolve(cache_name);

                if (verbose) {
                    std::cout << "NAME: " << name << "\n"
                              << "  VERSION: " <<  dep.version << "\n"
                              << "  SOURCE: " << dep.source << "\n"
                              << "  CACHE_NAME: " << cache_name << "\n"
                              << "  CURRENT_NAME: " << current_name << "\n"
                              << "  IS_CACHED: " << is_cached << "\n"
                              << std::endl;
                }

                if (resolver::current::resolve(current_name)) {
                    ++exists_count;
                    continue;
                }
                else if (is_cached) {
                    const bool res = copy_to_current(cache_name, current_name);
                    if (!quite) {
                        echo_install_status(res, name, dep.version, dep.source);
                    }
                }
                else if (dep.source == "poac" || dep.source == "github") {
                    const auto pkg_dir = path::poac_cache_dir / cache_name;
                    const auto tar_dir = pkg_dir.string() + ".tar.gz";
                    std::string target;
                    std::string host;
                    if (dep.source == "poac") {
                        target = resolver::archive_url(name, dep.version);
                        host = POAC_STORAGE_HOST;
                    }
                    else {
                        target = resolver::github::archive_url(name, dep.version);
                        host = GITHUB_HOST;
                    }

                    io::network::get(target, tar_dir, POAC_STORAGE_HOST);
                    // If res is true, does not execute func. (short-circuit evaluation)
                    bool res = tb::extract_spec_rm(tar_dir, pkg_dir);
                    res = res || copy_to_current(cache_name, current_name);

                    if (!quite) {
                        echo_install_status(res, name, dep.version, dep.source);
                    }
                }
                else {
                    // If called this, it is a bug.
                    throw except::error("Unexcepted error");
                }
            }
            if (exists_count == static_cast<int>(deps.size())) {
                io::cli::echo(io::cli::to_yellow("WARN: ") + "Already installed");
            }
        }

        // YAML::Node -> Deps
        // TODO: To resolver？
        core::resolver::Deps resolve_packages(const YAML::Node& node) {
            namespace except = core::exception;
            namespace naming = core::naming;
            namespace resolver = core::resolver;

            resolver::Deps deps;

            // Even if a package of the same name is written, it is excluded.
            // However, it can not deal with duplication of other information (e.g. version etc.).
            for (const auto& [name, next_node] : node.as<std::map<std::string, YAML::Node>>()) {
                // itr->first: itr->second
                const auto [src, parsed_name] = naming::get_source(name);
                const auto interval = naming::get_version(next_node, src);

                if (src == "poac" || src == "github") {
                    deps.push_back({ parsed_name, interval, src });
                }
                else { // unknown source
                    throw except::error("Unknown source");
                }
            }
            return deps;
        }

        std::optional<YAML::Node>
        check_lock_timestamp(std::string_view timestamp) { // core/lock.hpp ??
            namespace yaml = io::file::yaml;
            if (const auto lock = yaml::load("poac.lock")) {
                if (const auto lock_timestamp = yaml::get<std::string>(*lock, "timestamp")) {
                    if (timestamp == *lock_timestamp) {
                        return *lock;
                    }
                }
            }
            return std::nullopt;
        }

        std::optional<std::map<std::string, YAML::Node>>
        load_locked_deps(std::string_view timestamp) {
            namespace yaml = io::file::yaml;
            if (const auto lock = check_lock_timestamp(timestamp)) {
                if (const auto locked_deps = yaml::get<std::map<std::string, YAML::Node>>(*lock, "dependencies")) {
                    return *locked_deps;
                }
            }
            return std::nullopt;
        }

        std::string get_yaml_timestamp() {
            namespace fs = boost::filesystem;
            namespace yaml = io::file::yaml;
            namespace except = core::exception;

            if (const auto op_filename = yaml::exists_config()) {
                boost::system::error_code error;
                const std::time_t last_time = fs::last_write_time(*op_filename, error);
                return std::to_string(last_time);
            }
            else {
                throw except::error(
                        "poac.yml does not exists.\n"
                        "Please execute `poac init` or `poac new $PROJNAME`.");
            }
        }


        template<typename VS, typename = std::enable_if_t<std::is_rvalue_reference_v<VS&&>>>
        void _main(VS&& argv) {
            namespace fs = boost::filesystem;
            namespace except = core::exception;
            namespace path = io::file::path;
            namespace yaml = io::file::yaml;
            namespace cli = io::cli;
            namespace resolver = core::resolver;


            fs::create_directories(path::poac_cache_dir);
            const auto node = yaml::load_config("deps");
            const std::string timestamp = get_yaml_timestamp();
            const bool quite = util::argparse::use_rm(argv, "-q", "--quite");
            // When used at the same time, --quite is given priority.
            const bool verbose = !quite && util::argparse::use_rm(argv, "-v", "--verbose");


            resolver::Resolved resolved_deps{};
            bool load_lock = false;
            if (const auto locked_deps = load_locked_deps(timestamp)) {
                for (const auto& [name, next_node] : *locked_deps) {
                    const auto version = *yaml::get<std::string>(next_node, "version");
                    const auto source = *yaml::get<std::string>(next_node, "source");
                    // この場合，lockファイルを書き換える必要はないため，resolved_deps.activatedに何かを書き込む必要はない
                    resolved_deps.backtracked[name] = { version, source };
                }
                load_lock = true;
            }


            if (!quite) {
                cli::echo(cli::to_status("Resolving packages..."));
            }
            resolver::Deps deps;
            if (!load_lock) {
                deps = resolve_packages(node.at("deps"));
            }


            // 依存関係の解決
            if (!quite) {
                cli::echo(cli::to_status("Resolving dependencies..."));
            }
            if (!load_lock) {
                resolved_deps = resolver::resolve(deps);
            }


            if (!quite) {
                cli::echo(cli::to_status("Fetching..."));
                cli::echo();
            }
            fs::create_directories(path::current_deps_dir);
            fetch_packages(resolved_deps.backtracked, quite, verbose);
            if (!quite) {
                cli::echo();
                cli::echo(cli::status_done());
            }

            if (!load_lock) {
                create_lock_file(timestamp, resolved_deps.activated);
            }
        }
    }

    struct install {
        static const std::string summary() {
            return "Install packages";
        }
        static const std::string options() {
            return "[-v | --verbose, -q | --quite]";
        }
        template<typename VS, typename = std::enable_if_t<std::is_rvalue_reference_v<VS&&>>>
        void operator()(VS&& argv) {
            _install::_main(std::move(argv));
        }
    };
} // end namespace
#endif // !POAC_SUBCMD_INSTALL_HPP

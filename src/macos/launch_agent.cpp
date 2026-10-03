// SPDX-License-Identifier: MIT
#include "macos/launch_agent.h"

#include <fstream>
#include <sstream>
#include <unistd.h>

#include "common/posix/posix.h"
#include "common/text.h"
#include "kcf/build_info.h"
#include "kcf/embedded_templates.h"

namespace kcf::macos {
namespace {

constexpr const char* kLaunchctl = "/bin/launchctl";

std::string domain() {
    return "gui/" + std::to_string(::getuid());
}

std::string service_target() {
    return domain() + "/" + launch_agent_label();
}

posix::ProcessResult launchctl(std::vector<std::string> args) {
    args.insert(args.begin(), kLaunchctl);
    return posix::run_process(args);
}

std::string first_line(const std::string& s) {
    const std::string trimmed(text::trim(s));
    return trimmed.substr(0, trimmed.find('\n'));
}

std::string xml_unescape(std::string s) {
    s = text::replace_all(std::move(s), "&lt;", "<");
    s = text::replace_all(std::move(s), "&gt;", ">");
    s = text::replace_all(std::move(s), "&quot;", "\"");
    s = text::replace_all(std::move(s), "&apos;", "'");
    return text::replace_all(std::move(s), "&amp;", "&");
}

}  // namespace

std::string launch_agent_label() {
    return build::kServiceId;
}

std::string render_launch_agent(const std::filesystem::path& executable, const std::filesystem::path& log_file) {
    std::string plist = templates::k_macos_launch_agent;
    plist = text::replace_all(std::move(plist), "{{LABEL}}", text::xml_escape(launch_agent_label()));
    plist = text::replace_all(std::move(plist), "{{EXECUTABLE}}", text::xml_escape(executable.string()));
    plist = text::replace_all(std::move(plist), "{{LOG_FILE}}", text::xml_escape(log_file.string()));
    return plist;
}

std::optional<std::filesystem::path> launch_agent_executable(const std::filesystem::path& plist) {
    std::ifstream in(plist);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string content = buffer.str();
    const std::size_t key = content.find("<key>ProgramArguments</key>");
    if (key == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t open = content.find("<string>", key);
    const std::size_t close = content.find("</string>", open);
    if (open == std::string::npos || close == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t start = open + std::string_view("<string>").size();
    std::filesystem::path path(xml_unescape(content.substr(start, close - start)));
    if (!path.is_absolute()) {
        return std::nullopt;
    }
    return path;
}

LaunchdJob query_job() {
    LaunchdJob job;
    const posix::ProcessResult result = launchctl({"print", service_target()});
    if (result.exit_code != 0) {
        return job;
    }
    job.loaded = true;
    // `launchctl print` output is meant for humans; only the "pid = N" line is relied upon, and
    // only as a fallback when the daemon's own status socket does not answer.
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string_view t = text::trim(line);
        if (t.rfind("pid = ", 0) == 0) {
            try {
                job.pid = std::stol(std::string(t.substr(6)));
            } catch (...) {
            }
            break;
        }
    }
    return job;
}

bool bootstrap(const std::filesystem::path& plist, std::string& error) {
    // A previous `launchctl disable` would make bootstrap fail; enabling is harmless otherwise.
    launchctl({"enable", service_target()});
    const posix::ProcessResult result = launchctl({"bootstrap", domain(), plist.string()});
    if (result.exit_code == 0) {
        return true;
    }
    if (query_job().loaded) {
        return true;  // already loaded (launchctl reports error 5/37 in that case)
    }
    error = "launchctl bootstrap failed: " + first_line(result.output);
    return false;
}

bool bootout(std::string& error) {
    if (!query_job().loaded) {
        return true;
    }
    const posix::ProcessResult result = launchctl({"bootout", service_target()});
    if (result.exit_code == 0 || !query_job().loaded) {
        return true;
    }
    error = "launchctl bootout failed: " + first_line(result.output);
    return false;
}

bool kickstart(bool kill_running, std::string& error) {
    std::vector<std::string> args{"kickstart"};
    if (kill_running) {
        args.emplace_back("-k");
    }
    args.push_back(service_target());
    const posix::ProcessResult result = launchctl(args);
    if (result.exit_code == 0) {
        return true;
    }
    error = "launchctl kickstart failed: " + first_line(result.output);
    return false;
}

bool send_signal(const char* signal_name, std::string& error) {
    const posix::ProcessResult result = launchctl({"kill", signal_name, service_target()});
    if (result.exit_code == 0) {
        return true;
    }
    error = "launchctl kill failed: " + first_line(result.output);
    return false;
}

}  // namespace kcf::macos

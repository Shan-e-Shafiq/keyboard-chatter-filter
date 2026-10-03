// SPDX-License-Identifier: MIT
#include "common/status.h"

#include <charconv>
#include <sstream>

#include "common/text.h"

namespace kcf {
namespace {

std::string sanitize(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        out.push_back(c == '\n' || c == '\r' ? ' ' : c);
    }
    return out;
}

template <typename T>
bool parse_number(std::string_view text, T& out) {
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && ptr == text.data() + text.size();
}

std::string format_uptime(std::int64_t seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    std::ostringstream out;
    const std::int64_t days = seconds / 86400;
    const std::int64_t hours = (seconds / 3600) % 24;
    const std::int64_t minutes = (seconds / 60) % 60;
    if (days > 0) {
        out << days << "d ";
    }
    if (days > 0 || hours > 0) {
        out << hours << "h ";
    }
    out << minutes << "m";
    return out.str();
}

}  // namespace

const char* to_string(DaemonState state) noexcept {
    switch (state) {
        case DaemonState::Starting: return "starting";
        case DaemonState::Active: return "active";
        case DaemonState::WaitingForPermission: return "waiting-for-permission";
        case DaemonState::Disabled: return "disabled";
        case DaemonState::Error: return "error";
    }
    return "error";
}

std::optional<DaemonState> parse_daemon_state(std::string_view text) noexcept {
    for (const DaemonState s : {DaemonState::Starting, DaemonState::Active, DaemonState::WaitingForPermission,
                                DaemonState::Disabled, DaemonState::Error}) {
        if (text == to_string(s)) {
            return s;
        }
    }
    return std::nullopt;
}

std::string serialize_status(const DaemonStatus& s) {
    std::ostringstream out;
    out << "version=" << sanitize(s.version) << '\n'
        << "pid=" << s.pid << '\n'
        << "state=" << to_string(s.state) << '\n'
        << "detail=" << sanitize(s.detail) << '\n'
        << "threshold_ms=" << s.threshold_ms << '\n'
        << "enabled=" << (s.enabled ? "true" : "false") << '\n'
        << "devices=" << s.devices << '\n'
        << "suppressed=" << s.suppressed << '\n'
        << "config_path=" << sanitize(s.config_path) << '\n'
        << "config_problems=" << s.config_problems << '\n'
        << "uptime_seconds=" << s.uptime_seconds << '\n'
        << "dry_run=" << (s.dry_run ? "true" : "false") << '\n'
        << "safety_stop=" << sanitize(s.safety_stop) << '\n'
        << "chatter_gaps=" << s.chatter_gaps[0] << ',' << s.chatter_gaps[1] << ',' << s.chatter_gaps[2] << ','
        << s.chatter_gaps[3] << '\n'
        << "timing=" << sanitize(s.timing) << '\n';
    return out.str();
}

std::optional<DaemonStatus> parse_status(std::string_view text) {
    DaemonStatus s;
    bool saw_state = false;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        const std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        bool ok = true;
        if (key == "version") {
            s.version = std::string(value);
        } else if (key == "pid") {
            ok = parse_number(value, s.pid);
        } else if (key == "state") {
            const auto state = parse_daemon_state(value);
            ok = state.has_value();
            if (state) {
                s.state = *state;
                saw_state = true;
            }
        } else if (key == "detail") {
            s.detail = std::string(value);
        } else if (key == "threshold_ms") {
            ok = parse_number(value, s.threshold_ms);
        } else if (key == "enabled") {
            s.enabled = value == "true";
        } else if (key == "devices") {
            ok = parse_number(value, s.devices);
        } else if (key == "suppressed") {
            ok = parse_number(value, s.suppressed);
        } else if (key == "config_path") {
            s.config_path = std::string(value);
        } else if (key == "config_problems") {
            ok = parse_number(value, s.config_problems);
        } else if (key == "uptime_seconds") {
            ok = parse_number(value, s.uptime_seconds);
        } else if (key == "dry_run") {
            s.dry_run = value == "true";
        } else if (key == "safety_stop") {
            s.safety_stop = std::string(value);
        } else if (key == "chatter_gaps") {
            std::string_view rest = value;
            for (int i = 0; i < 4 && ok; ++i) {
                const std::size_t comma = rest.find(',');
                ok = parse_number(rest.substr(0, comma), s.chatter_gaps[i]);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
        } else if (key == "timing") {
            s.timing = std::string(value);
        }
        // Unknown keys are ignored so newer daemons can add fields.
        if (!ok) {
            return std::nullopt;
        }
    }
    if (!saw_state) {
        return std::nullopt;
    }
    return s;
}

std::string describe_status(const DaemonStatus& s) {
    std::ostringstream out;
    out << "  State:       " << to_string(s.state);
    if (!s.detail.empty()) {
        out << " - " << s.detail;
    }
    out << '\n';
    out << "  Version:     " << s.version << " (pid " << s.pid << ", up " << format_uptime(s.uptime_seconds) << ")\n";
    out << "  Filtering:   " << (s.enabled ? "enabled" : "disabled") << ", threshold " << s.threshold_ms << " ms\n";
    if (s.devices >= 0) {
        out << "  Keyboards:   " << s.devices << '\n';
    }
    if (!s.safety_stop.empty()) {
        out << "  SAFETY STOP: filtering was switched off automatically (" << s.safety_stop
            << "). The keyboard is unfiltered. Please report this; 'restart' re-arms it.\n";
    }
    if (s.dry_run) {
        out << "  Dry run:     observing only; nothing is dropped. Counts show what would be removed.\n";
    }
    out << "  Suppressed:  " << s.suppressed << " chatter keystroke(s) since start";
    if (s.suppressed > 0) {
        out << " (gaps <5 ms: " << s.chatter_gaps[0] << ", 5-10: " << s.chatter_gaps[1] << ", 10-20: "
            << s.chatter_gaps[2] << ", >=20: " << s.chatter_gaps[3] << ")";
    }
    out << '\n';
    if (!s.timing.empty()) {
        out << "  Timing:      " << s.timing << '\n';
    }
    out << "  Config:      " << (s.config_path.empty() ? "(defaults)" : s.config_path);
    if (s.config_problems > 0) {
        out << " (" << s.config_problems << " problem(s); see the log or run `keyboard-chatter-filter config`)";
    }
    out << '\n';
    return out.str();
}

}  // namespace kcf

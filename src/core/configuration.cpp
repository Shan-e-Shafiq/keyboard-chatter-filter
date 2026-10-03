// SPDX-License-Identifier: MIT
#include "keyboard_filter/configuration.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace kcf {
namespace {

using Severity = ConfigDiagnostic::Severity;

struct Value {
    enum class Type { Integer, Boolean, String, StringArray };
    Type type = Type::Integer;
    std::int64_t integer = 0;
    bool boolean = false;
    std::string string;
    std::vector<std::string> strings;
};

const char* type_name(Value::Type type) {
    switch (type) {
        case Value::Type::Integer: return "an integer";
        case Value::Type::Boolean: return "a boolean";
        case Value::Type::String: return "a string";
        case Value::Type::StringArray: return "an array of strings";
    }
    return "a value";
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool is_bare_key_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
}

// Parses a basic ("...") or literal ('...') string starting at text[pos]. On success advances pos
// past the closing quote.
std::optional<std::string> parse_string(std::string_view text, std::size_t& pos, std::string& error) {
    const char quote = text[pos];
    std::string out;
    std::size_t i = pos + 1;
    while (i < text.size()) {
        const char c = text[i];
        if (c == quote) {
            pos = i + 1;
            return out;
        }
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
            error = "control characters are not allowed in strings";
            return std::nullopt;
        }
        if (c == '\\' && quote == '"') {
            if (i + 1 >= text.size()) {
                break;
            }
            const char e = text[i + 1];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 't': out.push_back('\t'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                default:
                    error = std::string("unsupported escape sequence \\") + e;
                    return std::nullopt;
            }
            i += 2;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    error = "unterminated string";
    return std::nullopt;
}

std::optional<std::int64_t> parse_integer(std::string_view token) {
    std::string digits;
    std::size_t i = 0;
    if (!token.empty() && (token[0] == '+' || token[0] == '-')) {
        digits.push_back(token[0]);
        i = 1;
    }
    if (i >= token.size()) {
        return std::nullopt;
    }
    bool previous_digit = false;
    for (; i < token.size(); ++i) {
        const char c = token[i];
        if (c == '_') {
            // TOML allows underscores only between digits.
            if (!previous_digit || i + 1 >= token.size() ||
                std::isdigit(static_cast<unsigned char>(token[i + 1])) == 0) {
                return std::nullopt;
            }
            previous_digit = false;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return std::nullopt;
        }
        digits.push_back(c);
        previous_digit = true;
    }
    std::int64_t value = 0;
    const char* begin = digits.data() + (digits[0] == '+' ? 1 : 0);
    const auto [ptr, ec] = std::from_chars(begin, digits.data() + digits.size(), value);
    if (ec != std::errc() || ptr != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return value;
}

// Parses the value part of a `key = value` line, including any trailing comment.
std::optional<Value> parse_value(std::string_view text, std::string& error) {
    std::size_t pos = 0;
    Value value;

    auto rest_is_comment_or_empty = [&](std::size_t from) {
        const std::string_view rest = trim(text.substr(from));
        return rest.empty() || rest.front() == '#';
    };

    if (text.empty()) {
        error = "missing value";
        return std::nullopt;
    }

    if (text[0] == '"' || text[0] == '\'') {
        if (text.substr(0, 3) == "\"\"\"" || text.substr(0, 3) == "'''") {
            error = "multi-line strings are not supported";
            return std::nullopt;
        }
        auto s = parse_string(text, pos, error);
        if (!s) {
            return std::nullopt;
        }
        if (!rest_is_comment_or_empty(pos)) {
            error = "unexpected text after value";
            return std::nullopt;
        }
        value.type = Value::Type::String;
        value.string = std::move(*s);
        return value;
    }

    if (text[0] == '[') {
        value.type = Value::Type::StringArray;
        pos = 1;
        bool expect_item = true;
        while (true) {
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
                ++pos;
            }
            if (pos >= text.size() || text[pos] == '#') {
                error = "arrays must be written on a single line and closed with ']'";
                return std::nullopt;
            }
            if (text[pos] == ']') {
                ++pos;
                break;
            }
            if (!expect_item) {
                if (text[pos] != ',') {
                    error = "expected ',' or ']' in array";
                    return std::nullopt;
                }
                ++pos;
                expect_item = true;
                continue;
            }
            if (text[pos] != '"' && text[pos] != '\'') {
                error = "only arrays of strings are supported";
                return std::nullopt;
            }
            auto s = parse_string(text, pos, error);
            if (!s) {
                return std::nullopt;
            }
            value.strings.push_back(std::move(*s));
            expect_item = false;
        }
        if (!rest_is_comment_or_empty(pos)) {
            error = "unexpected text after value";
            return std::nullopt;
        }
        return value;
    }

    if (text[0] == '{') {
        error = "inline tables are not supported";
        return std::nullopt;
    }

    // Bare token: integer or boolean. Ends at whitespace or a comment.
    std::size_t end = 0;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '#') {
        ++end;
    }
    const std::string_view token = text.substr(0, end);
    if (!rest_is_comment_or_empty(end)) {
        error = "unexpected text after value";
        return std::nullopt;
    }
    if (token == "true" || token == "false") {
        value.type = Value::Type::Boolean;
        value.boolean = token == "true";
        return value;
    }
    if (auto integer = parse_integer(token)) {
        value.type = Value::Type::Integer;
        value.integer = *integer;
        return value;
    }
    const bool numeric = !token.empty() && std::all_of(token.begin(), token.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-' || c == '_';
    });
    if (numeric) {
        error = "'" + std::string(token) + "' is not a valid integer";
    } else if (token.find('.') != std::string_view::npos || token.find('e') != std::string_view::npos) {
        error = "'" + std::string(token) + "' is not an integer (fractions are not supported)";
    } else {
        error = "'" + std::string(token) + "' is not a valid value (strings must be quoted)";
    }
    return std::nullopt;
}

bool ascii_iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

class Parser {
public:
    ConfigLoadResult run(std::string_view text) {
        if (text.substr(0, 3) == "\xEF\xBB\xBF") {
            text.remove_prefix(3);
        }
        std::size_t line_no = 0;
        while (!text.empty()) {
            ++line_no;
            const std::size_t newline = text.find('\n');
            const std::string_view line = text.substr(0, newline);
            text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
            parse_line(trim(line), line_no);
        }
        return std::move(result_);
    }

private:
    void report(Severity severity, std::size_t line, std::string message) {
        result_.diagnostics.push_back({severity, line, std::move(message)});
    }

    void parse_line(std::string_view line, std::size_t line_no) {
        if (line.empty() || line.front() == '#') {
            return;
        }
        if (line.front() == '[') {
            report(Severity::Error, line_no, "tables are not supported; all settings are top-level keys");
            in_unsupported_table_ = true;
            return;
        }
        if (in_unsupported_table_) {
            return;  // keys inside an unsupported table were already reported with the table
        }

        std::size_t key_end = 0;
        while (key_end < line.size() && is_bare_key_char(line[key_end])) {
            ++key_end;
        }
        const std::string key(line.substr(0, key_end));
        std::string_view rest = trim(line.substr(key_end));
        if (key.empty() || rest.empty() || rest.front() != '=') {
            report(Severity::Error, line_no, "expected 'key = value'");
            return;
        }
        rest = trim(rest.substr(1));

        if (!seen_keys_.insert(key).second) {
            report(Severity::Error, line_no, "duplicate key '" + key + "' (the first value is used)");
            return;
        }

        std::string error;
        const std::optional<Value> value = parse_value(rest, error);
        if (!value) {
            report(Severity::Error, line_no, "invalid value for '" + key + "': " + error + "; using the default");
            return;
        }
        apply(key, *value, line_no);
    }

    bool expect(const std::string& key, const Value& value, Value::Type type, std::size_t line_no) {
        if (value.type == type) {
            return true;
        }
        report(Severity::Error, line_no,
               "'" + key + "' must be " + type_name(type) + ", got " + type_name(value.type) + "; using the default");
        return false;
    }

    void apply(const std::string& key, const Value& value, std::size_t line_no) {
        Configuration& config = result_.config;
        if (key == "chatter_threshold_ms") {
            if (!expect(key, value, Value::Type::Integer, line_no)) {
                return;
            }
            const auto min = Configuration::kMinThreshold.count();
            const auto max = Configuration::kMaxThreshold.count();
            if (value.integer < min || value.integer > max) {
                report(Severity::Error, line_no,
                       "chatter_threshold_ms must be between " + std::to_string(min) + " and " + std::to_string(max) +
                           ", got " + std::to_string(value.integer) + "; using the default of " +
                           std::to_string(Configuration::kDefaultThreshold.count()));
                return;
            }
            config.chatter_threshold = std::chrono::milliseconds(value.integer);
            if (config.chatter_threshold > Configuration::kHighThresholdWarning) {
                report(Severity::Warning, line_no,
                       "chatter_threshold_ms above " + std::to_string(Configuration::kHighThresholdWarning.count()) +
                           " may merge intentional fast double letters");
            }
        } else if (key == "enabled") {
            if (expect(key, value, Value::Type::Boolean, line_no)) {
                config.enabled = value.boolean;
            }
        } else if (key == "log_level") {
            if (!expect(key, value, Value::Type::String, line_no)) {
                return;
            }
            if (const auto level = parse_log_level(value.string)) {
                config.log_level = *level;
            } else {
                report(Severity::Error, line_no,
                       "log_level must be one of \"error\", \"warning\", \"info\", \"debug\", got \"" + value.string +
                           "\"; using the default");
            }
        } else if (key == "ignored_devices") {
            if (!expect(key, value, Value::Type::StringArray, line_no)) {
                return;
            }
            std::vector<std::string> devices;
            for (const std::string& name : value.strings) {
                if (name.empty() || name.size() > 128) {
                    report(Severity::Error, line_no, "ignored_devices entries must be 1 to 128 characters; entry skipped");
                    continue;
                }
                devices.push_back(name);
            }
            config.ignored_devices = std::move(devices);
        } else {
            report(Severity::Warning, line_no, "unknown setting '" + key + "' ignored");
        }
    }

    ConfigLoadResult result_;
    std::set<std::string, std::less<>> seen_keys_;
    bool in_unsupported_table_ = false;
};

}  // namespace

const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Error: return "error";
        case LogLevel::Warning: return "warning";
        case LogLevel::Info: return "info";
        case LogLevel::Debug: return "debug";
    }
    return "info";
}

std::optional<LogLevel> parse_log_level(std::string_view text) noexcept {
    if (ascii_iequals(text, "error")) {
        return LogLevel::Error;
    }
    if (ascii_iequals(text, "warning") || ascii_iequals(text, "warn")) {
        return LogLevel::Warning;
    }
    if (ascii_iequals(text, "info")) {
        return LogLevel::Info;
    }
    if (ascii_iequals(text, "debug")) {
        return LogLevel::Debug;
    }
    return std::nullopt;
}

bool ConfigLoadResult::has_errors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const ConfigDiagnostic& d) { return d.severity == Severity::Error; });
}

std::vector<std::string> ConfigLoadResult::describe(const std::filesystem::path& path) const {
    std::vector<std::string> lines;
    lines.reserve(diagnostics.size());
    for (const ConfigDiagnostic& d : diagnostics) {
        std::ostringstream out;
        out << path.string();
        if (d.line != 0) {
            out << ':' << d.line;
        }
        out << ": " << (d.severity == Severity::Error ? "error" : "warning") << ": " << d.message;
        lines.push_back(out.str());
    }
    return lines;
}

ConfigLoadResult parse_configuration(std::string_view text) {
    return Parser{}.run(text);
}

ConfigLoadResult load_configuration(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status)) {
        return {};  // no file: defaults
    }

    ConfigLoadResult failed;
    failed.file_found = true;
    if (!std::filesystem::is_regular_file(status)) {
        failed.diagnostics.push_back({Severity::Error, 0, "not a regular file; using defaults"});
        return failed;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        failed.diagnostics.push_back({Severity::Error, 0, "cannot determine file size (" + ec.message() + "); using defaults"});
        return failed;
    }
    if (size > kMaxConfigFileBytes) {
        failed.diagnostics.push_back(
            {Severity::Error, 0,
             "file is larger than " + std::to_string(kMaxConfigFileBytes) + " bytes; using defaults"});
        return failed;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        failed.diagnostics.push_back({Severity::Error, 0, "cannot be read; using defaults"});
        return failed;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));

    ConfigLoadResult result = parse_configuration(text);
    result.file_found = true;
    return result;
}

std::string default_configuration_text() {
    return R"(# keyboard-chatter-filter configuration
#
# Changes take effect after `keyboard-chatter-filter reload` (macOS/Linux) or
# `keyboard-chatter-filter restart`. Invalid values are reported in the log and
# replaced by their defaults.

# A key that is released and pressed again within this many milliseconds is
# treated as switch chatter and the duplicate keystroke is removed.
# Range 1-200. Raise it if doubled letters still slip through; lower it if
# intentional fast double-taps of the same key get lost.
chatter_threshold_ms = 30

# Set to false to keep the service running without filtering anything.
enabled = true

# One of "error", "warning", "info", "debug". Keystrokes are never logged.
log_level = "info"

# Linux only: never intercept input devices whose name contains one of these
# strings (case-insensitive), e.g. hardware tokens that type very quickly.
# ignored_devices = ["YubiKey"]
)";
}

}  // namespace kcf

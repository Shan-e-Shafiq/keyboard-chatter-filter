// SPDX-License-Identifier: MIT
#include <filesystem>
#include <fstream>
#include <random>

#include "keyboard_filter/configuration.h"
#include "test_framework.h"

using namespace kcf;
using namespace std::chrono_literals;

namespace {

using Severity = ConfigDiagnostic::Severity;

std::size_t errors(const ConfigLoadResult& r) {
    std::size_t n = 0;
    for (const auto& d : r.diagnostics) {
        n += d.severity == Severity::Error ? 1 : 0;
    }
    return n;
}

std::size_t warnings(const ConfigLoadResult& r) {
    return r.diagnostics.size() - errors(r);
}

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() / ("kcf-config-test-" + std::to_string(rd()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path write(const std::string& name, const std::string& content) const {
        const auto file = path_ / name;
        std::ofstream(file, std::ios::binary) << content;
        return file;
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST(Config, EmptyTextGivesDefaults) {
    const auto r = parse_configuration("");
    EXPECT_TRUE(r.diagnostics.empty());
    EXPECT_TRUE(r.config == Configuration{});
    EXPECT_EQ(r.config.chatter_threshold, 30ms);
    EXPECT_TRUE(r.config.enabled);
    EXPECT_EQ(r.config.log_level, LogLevel::Info);
}

TEST(Config, ValidFile) {
    const auto r = parse_configuration(
        "# comment\n"
        "chatter_threshold_ms = 45\n"
        "enabled = false   # trailing comment\n"
        "log_level = \"debug\"\n"
        "ignored_devices = [\"YubiKey\", 'Barcode Scanner']\n");
    EXPECT_TRUE(r.diagnostics.empty());
    EXPECT_EQ(r.config.chatter_threshold, 45ms);
    EXPECT_FALSE(r.config.enabled);
    EXPECT_EQ(r.config.log_level, LogLevel::Debug);
    ASSERT_EQ(r.config.ignored_devices.size(), 2u);
    EXPECT_EQ(r.config.ignored_devices[0], "YubiKey");
    EXPECT_EQ(r.config.ignored_devices[1], "Barcode Scanner");
}

TEST(Config, WhitespaceCrlfAndBom) {
    const auto r = parse_configuration("\xEF\xBB\xBF  chatter_threshold_ms=12\r\n\tenabled\t=\ttrue\r\n\r\n");
    EXPECT_TRUE(r.diagnostics.empty());
    EXPECT_EQ(r.config.chatter_threshold, 12ms);
    EXPECT_TRUE(r.config.enabled);
}

TEST(Config, FilterSettingsReflectConfiguration) {
    const auto r = parse_configuration("chatter_threshold_ms = 18\nenabled = false\n");
    const FilterSettings s = r.config.filter_settings();
    EXPECT_EQ(s.threshold, 18ms);
    EXPECT_FALSE(s.enabled);
}

TEST(Config, ThresholdBoundsAreInclusive) {
    EXPECT_EQ(parse_configuration("chatter_threshold_ms = 1").config.chatter_threshold, 1ms);
    EXPECT_EQ(parse_configuration("chatter_threshold_ms = 200").config.chatter_threshold, 200ms);
}

TEST(Config, ThresholdOutOfRangeFallsBackToDefault) {
    for (const char* text : {"chatter_threshold_ms = 0", "chatter_threshold_ms = 201", "chatter_threshold_ms = -5",
                             "chatter_threshold_ms = 99999999999999999999"}) {
        const auto r = parse_configuration(text);
        EXPECT_EQ(errors(r), 1u);
        EXPECT_EQ(r.config.chatter_threshold, Configuration::kDefaultThreshold);
        EXPECT_TRUE(r.has_errors());
    }
}

TEST(Config, HighThresholdIsAcceptedWithWarning) {
    const auto r = parse_configuration("chatter_threshold_ms = 120");
    EXPECT_EQ(r.config.chatter_threshold, 120ms);
    EXPECT_EQ(errors(r), 0u);
    EXPECT_EQ(warnings(r), 1u);
    EXPECT_FALSE(r.has_errors());
}

TEST(Config, ThresholdWrongTypes) {
    for (const char* text : {"chatter_threshold_ms = \"30\"", "chatter_threshold_ms = 30.5",
                             "chatter_threshold_ms = true", "chatter_threshold_ms = 3e1",
                             "chatter_threshold_ms = thirty", "chatter_threshold_ms = [30]"}) {
        const auto r = parse_configuration(text);
        EXPECT_EQ(errors(r), 1u);
        EXPECT_EQ(r.config.chatter_threshold, Configuration::kDefaultThreshold);
    }
}

TEST(Config, IntegerSyntax) {
    EXPECT_EQ(parse_configuration("chatter_threshold_ms = +25").config.chatter_threshold, 25ms);
    EXPECT_EQ(parse_configuration("chatter_threshold_ms = 1_5").config.chatter_threshold, 15ms);
    EXPECT_TRUE(parse_configuration("chatter_threshold_ms = 1__5").has_errors());
    EXPECT_TRUE(parse_configuration("chatter_threshold_ms = _15").has_errors());
    EXPECT_TRUE(parse_configuration("chatter_threshold_ms = 15_").has_errors());
    EXPECT_TRUE(parse_configuration("chatter_threshold_ms = +").has_errors());
}

TEST(Config, EnabledMustBeBoolean) {
    for (const char* text : {"enabled = 1", "enabled = \"true\"", "enabled = yes", "enabled = TRUE"}) {
        const auto r = parse_configuration(text);
        EXPECT_EQ(errors(r), 1u);
        EXPECT_TRUE(r.config.enabled);
    }
}

TEST(Config, LogLevels) {
    EXPECT_EQ(parse_configuration("log_level = \"error\"").config.log_level, LogLevel::Error);
    EXPECT_EQ(parse_configuration("log_level = \"warning\"").config.log_level, LogLevel::Warning);
    EXPECT_EQ(parse_configuration("log_level = \"warn\"").config.log_level, LogLevel::Warning);
    EXPECT_EQ(parse_configuration("log_level = \"INFO\"").config.log_level, LogLevel::Info);
    EXPECT_EQ(parse_configuration("log_level = 'Debug'").config.log_level, LogLevel::Debug);

    const auto bad = parse_configuration("log_level = \"verbose\"");
    EXPECT_EQ(errors(bad), 1u);
    EXPECT_EQ(bad.config.log_level, LogLevel::Info);
    EXPECT_TRUE(parse_configuration("log_level = debug").has_errors());  // unquoted
}

TEST(Config, UnknownKeysWarnButOthersApply) {
    const auto r = parse_configuration("colour = \"blue\"\nchatter_threshold_ms = 40\n");
    EXPECT_EQ(errors(r), 0u);
    EXPECT_EQ(warnings(r), 1u);
    EXPECT_EQ(r.config.chatter_threshold, 40ms);
    EXPECT_EQ(r.diagnostics[0].line, 1u);
}

TEST(Config, DuplicateKeyKeepsFirstValue) {
    const auto r = parse_configuration("chatter_threshold_ms = 40\nchatter_threshold_ms = 50\n");
    EXPECT_EQ(errors(r), 1u);
    EXPECT_EQ(r.diagnostics[0].line, 2u);
    EXPECT_EQ(r.config.chatter_threshold, 40ms);
}

TEST(Config, TablesAreRejectedTogetherWithTheirKeys) {
    const auto r = parse_configuration("enabled = false\n[advanced]\nchatter_threshold_ms = 5\n");
    EXPECT_EQ(errors(r), 1u);
    EXPECT_FALSE(r.config.enabled);
    EXPECT_EQ(r.config.chatter_threshold, Configuration::kDefaultThreshold);
}

TEST(Config, MalformedLines) {
    for (const char* text : {"chatter_threshold_ms 30", "= 5", "chatter_threshold_ms =", "\"quoted\" = 5",
                             "a.b = 5", "chatter_threshold_ms = 30 40", "log_level = \"info\" extra",
                             "log_level = \"unterminated", "log_level = \"bad \\q escape\"",
                             "log_level = \"\"\"multi\"\"\"", "ignored_devices = [\"a\"", "ignored_devices = [1, 2]",
                             "ignored_devices = [\"a\" \"b\"]", "enabled = {a = 1}"}) {
        const auto r = parse_configuration(text);
        EXPECT_TRUE(r.has_errors());
        EXPECT_TRUE(r.config == Configuration{});
    }
}

TEST(Config, ErrorsDoNotAffectOtherLines) {
    const auto r = parse_configuration("chatter_threshold_ms = 999\nenabled = false\nlog_level = \"nope\"\n");
    EXPECT_EQ(errors(r), 2u);
    EXPECT_EQ(r.config.chatter_threshold, Configuration::kDefaultThreshold);
    EXPECT_FALSE(r.config.enabled);
    EXPECT_EQ(r.config.log_level, LogLevel::Info);
}

TEST(Config, StringEscapesAndComments) {
    const auto r = parse_configuration("ignored_devices = [\"Key # board\", \"say \\\"hi\\\"\"] # comment\n");
    EXPECT_TRUE(r.diagnostics.empty());
    ASSERT_EQ(r.config.ignored_devices.size(), 2u);
    EXPECT_EQ(r.config.ignored_devices[0], "Key # board");
    EXPECT_EQ(r.config.ignored_devices[1], "say \"hi\"");
}

TEST(Config, IgnoredDevicesValidation) {
    EXPECT_TRUE(parse_configuration("ignored_devices = []").config.ignored_devices.empty());
    const auto r = parse_configuration("ignored_devices = [\"\", \"ok\"]");
    EXPECT_EQ(errors(r), 1u);
    ASSERT_EQ(r.config.ignored_devices.size(), 1u);
    EXPECT_EQ(r.config.ignored_devices[0], "ok");
    EXPECT_TRUE(parse_configuration("ignored_devices = \"YubiKey\"").has_errors());
}

TEST(Config, DescribeIncludesPathAndLine) {
    const auto r = parse_configuration("\nenabled = 3\n");
    const auto lines = r.describe("/etc/kcf.toml");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].rfind("/etc/kcf.toml:2: error: ", 0), 0u);
}

TEST(Config, DefaultTemplateParsesToDefaults) {
    const auto r = parse_configuration(default_configuration_text());
    EXPECT_TRUE(r.diagnostics.empty());
    EXPECT_TRUE(r.config == Configuration{});
}

TEST(ConfigFile, MissingFileGivesDefaultsWithoutDiagnostics) {
    TempDir dir;
    const auto r = load_configuration(dir.path() / "does-not-exist.toml");
    EXPECT_FALSE(r.file_found);
    EXPECT_TRUE(r.diagnostics.empty());
    EXPECT_TRUE(r.config == Configuration{});
}

TEST(ConfigFile, LoadsValidFile) {
    TempDir dir;
    const auto path = dir.write("config.toml", "chatter_threshold_ms = 22\nlog_level = \"warning\"\n");
    const auto r = load_configuration(path);
    EXPECT_TRUE(r.file_found);
    EXPECT_TRUE(r.diagnostics.empty());
    EXPECT_EQ(r.config.chatter_threshold, 22ms);
    EXPECT_EQ(r.config.log_level, LogLevel::Warning);
}

TEST(ConfigFile, DirectoryIsReportedAndDefaultsApply) {
    TempDir dir;
    std::filesystem::create_directories(dir.path() / "config.toml");
    const auto r = load_configuration(dir.path() / "config.toml");
    EXPECT_TRUE(r.file_found);
    EXPECT_TRUE(r.has_errors());
    EXPECT_TRUE(r.config == Configuration{});
}

TEST(ConfigFile, OversizedFileIsRejected) {
    TempDir dir;
    std::string big = "chatter_threshold_ms = 10\n";
    big += std::string(kMaxConfigFileBytes, '#');
    const auto r = load_configuration(dir.write("config.toml", big));
    EXPECT_TRUE(r.has_errors());
    EXPECT_EQ(r.config.chatter_threshold, Configuration::kDefaultThreshold);
}

TEST(ConfigFile, BinaryGarbageDoesNotCrash) {
    TempDir dir;
    std::string garbage;
    std::mt19937 rng(1234);
    for (int i = 0; i < 4096; ++i) {
        garbage.push_back(static_cast<char>(rng() & 0xFF));
    }
    const auto r = load_configuration(dir.write("config.toml", garbage));
    EXPECT_TRUE(r.file_found);
    EXPECT_GE(r.config.chatter_threshold, Configuration::kMinThreshold);
    EXPECT_LE(r.config.chatter_threshold, Configuration::kMaxThreshold);
}

TEST(ConfigFile, RandomisedInputNeverYieldsInvalidSettings) {
    const std::string alphabet = "chatter_threshold_ms=enabledlog_level\"'[]#, \n0123456789-+truefalse.\\";
    std::mt19937 rng(99);
    for (int round = 0; round < 2000; ++round) {
        std::string text;
        const int length = static_cast<int>(rng() % 120);
        for (int i = 0; i < length; ++i) {
            text.push_back(alphabet[rng() % alphabet.size()]);
        }
        const auto r = parse_configuration(text);
        EXPECT_GE(r.config.chatter_threshold, Configuration::kMinThreshold);
        EXPECT_LE(r.config.chatter_threshold, Configuration::kMaxThreshold);
    }
}

KCF_TEST_MAIN()

// SPDX-License-Identifier: MIT
#include <string>
#include <string_view>
#include <vector>

#include "common/cli.h"
#include "common/status.h"
#include "common/text.h"
#include "kcf/embedded_templates.h"
#include "test_framework.h"

using namespace kcf;

namespace {

CliParseResult parse(std::vector<std::string_view> args) {
    return parse_command_line(args);
}

}  // namespace

TEST(Cli, NoArgumentsShowsHelp) {
    const auto r = parse({});
    ASSERT_TRUE(r.options.has_value());
    EXPECT_TRUE(r.options->command == Command::Help);
}

TEST(Cli, Commands) {
    const std::vector<std::pair<std::string_view, Command>> cases{
        {"run", Command::Run},         {"start", Command::Start},         {"stop", Command::Stop},
        {"restart", Command::Restart}, {"reload", Command::Reload},       {"status", Command::Status},
        {"install", Command::Install}, {"uninstall", Command::Uninstall}, {"config", Command::Config},
        {"version", Command::Version}, {"--version", Command::Version},   {"help", Command::Help},
        {"-h", Command::Help},
    };
    for (const auto& [name, command] : cases) {
        const auto r = parse({name});
        ASSERT_TRUE(r.options.has_value());
        EXPECT_TRUE(r.options->command == command);
    }
}

TEST(Cli, UnknownCommandIsAnError) {
    const auto r = parse({"frobnicate"});
    EXPECT_FALSE(r.options.has_value());
    EXPECT_NE(r.error.find("frobnicate"), std::string::npos);
}

TEST(Cli, ConfigOption) {
    auto r = parse({"run", "--config", "/tmp/x.toml"});
    ASSERT_TRUE(r.options.has_value());
    ASSERT_TRUE(r.options->config_path.has_value());
    EXPECT_EQ(r.options->config_path->string(), "/tmp/x.toml");

    r = parse({"config", "--config=/etc/y.toml"});
    ASSERT_TRUE(r.options.has_value());
    EXPECT_EQ(r.options->config_path->string(), "/etc/y.toml");

    EXPECT_FALSE(parse({"run", "--config"}).options.has_value());
    EXPECT_FALSE(parse({"run", "--config="}).options.has_value());
    EXPECT_FALSE(parse({"start", "--config", "/tmp/x"}).options.has_value());
}

TEST(Cli, LogLevelOption) {
    const auto r = parse({"run", "--log-level", "debug"});
    ASSERT_TRUE(r.options.has_value());
    EXPECT_TRUE(r.options->log_level == LogLevel::Debug);
    EXPECT_FALSE(parse({"run", "--log-level", "loud"}).options.has_value());
    EXPECT_FALSE(parse({"status", "--log-level", "debug"}).options.has_value());
}

TEST(Cli, PurgeOnlyForUninstall) {
    const auto r = parse({"uninstall", "--purge"});
    ASSERT_TRUE(r.options.has_value());
    EXPECT_TRUE(r.options->purge);
    EXPECT_FALSE(parse({"stop", "--purge"}).options.has_value());
}

TEST(Cli, StopEventOnlyForWindowsAgent) {
    const auto r = parse({"windows-agent", "--stop-event", "1234"});
    ASSERT_TRUE(r.options.has_value());
    EXPECT_TRUE(r.options->stop_event == std::optional<std::uint64_t>(1234));
    EXPECT_FALSE(parse({"run", "--stop-event", "1"}).options.has_value());
    EXPECT_FALSE(parse({"windows-agent", "--stop-event", "abc"}).options.has_value());
}

TEST(Cli, UnexpectedArgument) {
    EXPECT_FALSE(parse({"status", "extra"}).options.has_value());
}

TEST(Cli, UsageMentionsEveryCommand) {
    const std::string usage = usage_text();
    for (const char* word : {"status", "start", "stop", "restart", "reload", "install", "uninstall", "config", "run",
                             "version"}) {
        EXPECT_NE(usage.find(word), std::string::npos);
    }
}

TEST(Status, RoundTrip) {
    DaemonStatus s;
    s.version = "1.2.3";
    s.pid = 4242;
    s.state = DaemonState::WaitingForPermission;
    s.detail = "grant access\nplease";  // newlines must not break the line format
    s.threshold_ms = 25;
    s.enabled = false;
    s.devices = 2;
    s.suppressed = 17;
    s.config_path = "/home/u/.config/keyboard-chatter-filter/config.toml";
    s.config_problems = 1;
    s.uptime_seconds = 3700;

    const auto parsed = parse_status(serialize_status(s));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->version, "1.2.3");
    EXPECT_EQ(parsed->pid, 4242);
    EXPECT_TRUE(parsed->state == DaemonState::WaitingForPermission);
    EXPECT_EQ(parsed->detail, "grant access please");
    EXPECT_EQ(parsed->threshold_ms, 25);
    EXPECT_FALSE(parsed->enabled);
    EXPECT_EQ(parsed->devices, 2);
    EXPECT_EQ(parsed->suppressed, 17u);
    EXPECT_EQ(parsed->config_path, s.config_path);
    EXPECT_EQ(parsed->config_problems, 1);
    EXPECT_EQ(parsed->uptime_seconds, 3700);
}

TEST(Status, RejectsGarbage) {
    EXPECT_FALSE(parse_status("").has_value());
    EXPECT_FALSE(parse_status("hello world").has_value());
    EXPECT_FALSE(parse_status("state=dancing\n").has_value());
    EXPECT_FALSE(parse_status("state=active\npid=abc\n").has_value());
    EXPECT_TRUE(parse_status("state=active\nfuture_field=1\n").has_value());
}

TEST(Status, DescribeIsReadable) {
    DaemonStatus s;
    s.state = DaemonState::Active;
    s.version = "0.1.0";
    s.threshold_ms = 30;
    s.suppressed = 3;
    s.uptime_seconds = 90061;
    const std::string text = describe_status(s);
    EXPECT_NE(text.find("active"), std::string::npos);
    EXPECT_NE(text.find("30 ms"), std::string::npos);
    EXPECT_NE(text.find("1d 1h 1m"), std::string::npos);
    EXPECT_EQ(text.find("Keyboards"), std::string::npos);  // unknown device count is not shown
}

TEST(Text, Helpers) {
    EXPECT_EQ(text::trim("  a b \n"), "a b");
    EXPECT_TRUE(text::icontains("Yubico YubiKey OTP", "yubikey"));
    EXPECT_FALSE(text::icontains("Keychron", "yubikey"));
    EXPECT_EQ(text::xml_escape("a<b>&\"'"), "a&lt;b&gt;&amp;&quot;&apos;");
    EXPECT_EQ(text::replace_all("{{X}}-{{X}}", "{{X}}", "y"), "y-y");
    EXPECT_TRUE(text::is_safe_unquoted_path("/usr/local/bin/keyboard-chatter-filter"));
    EXPECT_FALSE(text::is_safe_unquoted_path("/opt/my tools/kcf"));
    EXPECT_FALSE(text::is_safe_unquoted_path("/opt/$HOME/kcf"));
    EXPECT_FALSE(text::is_safe_unquoted_path("/opt/a%b"));
    EXPECT_FALSE(text::is_safe_unquoted_path(""));
}

TEST(Templates, ContainPlaceholders) {
    const std::string plist = templates::k_macos_launch_agent;
    for (const char* p : {"{{LABEL}}", "{{EXECUTABLE}}", "{{LOG_FILE}}"}) {
        EXPECT_NE(plist.find(p), std::string::npos);
    }
    EXPECT_NE(plist.find("<key>KeepAlive</key>"), std::string::npos);
    EXPECT_NE(plist.find("<key>RunAtLoad</key>"), std::string::npos);

    const std::string unit = templates::k_linux_systemd_unit;
    for (const char* p : {"{{EXECUTABLE}}", "{{SERVICE_ID}}", "{{PROJECT_URL}}"}) {
        EXPECT_NE(unit.find(p), std::string::npos);
    }
    EXPECT_NE(unit.find("Restart=on-failure"), std::string::npos);
    EXPECT_NE(unit.find("PrivateNetwork=yes"), std::string::npos);
    EXPECT_NE(unit.find("WatchdogSec="), std::string::npos);
}

KCF_TEST_MAIN()

// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

namespace kcf::text {

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;
[[nodiscard]] bool icontains(std::string_view haystack, std::string_view needle) noexcept;
[[nodiscard]] std::string xml_escape(std::string_view s);

// Replaces every occurrence of `from` in `s` with `to`.
[[nodiscard]] std::string replace_all(std::string s, std::string_view from, std::string_view to);

// True if `s` can be embedded verbatim in a systemd unit file or launchd plist path without
// quoting: printable ASCII without whitespace, quotes, backslashes, '%' or '$'.
[[nodiscard]] bool is_safe_unquoted_path(std::string_view s) noexcept;

}  // namespace kcf::text

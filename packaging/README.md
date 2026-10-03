# Packaging

Service definitions for each platform. The templates are embedded into the binary at build time
(`cmake/EmbedTemplates.cmake`), so `keyboard-chatter-filter install` and these files can never
disagree. Placeholders in double braces are filled in at install time.

| Path | Used by |
|---|---|
| `macos/launch-agent.plist.in` | `keyboard-chatter-filter install` on macOS → `~/Library/LaunchAgents/keyboard-chatter-filter.plist` |
| `linux/keyboard-chatter-filter.service.in` | `keyboard-chatter-filter install` on Linux → `/etc/systemd/system/keyboard-chatter-filter.service` |
| `windows/` | reference for the service the Windows binary registers through the SCM API |

Distribution packagers can install these files directly instead of calling `install` (substitute
`{{EXECUTABLE}}`, `{{LABEL}}`/`{{SERVICE_ID}}` = `keyboard-chatter-filter`, `{{LOG_FILE}}`,
`{{PROJECT_URL}}`).

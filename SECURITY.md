# Security policy

keyboard-chatter-filter sees every keystroke on the machines it runs on, so security reports are
taken seriously.

## Reporting a vulnerability

Please **do not open a public issue**. Report privately through GitHub's
[private vulnerability reporting](https://docs.github.com/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
("Report a vulnerability" on the repository's Security tab). Include the affected version and
platform, and steps to reproduce if possible. You should receive an acknowledgement within a few
days.

## Scope

In scope, for example:

* any way the program could record, persist or disclose keystrokes or other typed content;
* privilege escalation through the service, the installers or file permissions;
* ways for another local user or process to influence the filter or the service;
* integrity problems in the release or installation pipeline.

## Security design

See [Security and privacy](README.md#security-and-privacy) in the README and the platform documents
in [docs/](docs/) for the threat model and the measures taken.

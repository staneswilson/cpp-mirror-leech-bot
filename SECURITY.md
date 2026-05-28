# Security Policy

CMLB takes security seriously. The bot handles Telegram credentials, file
transfers, and shell-adjacent tooling (aria2, qBittorrent, ffmpeg), so a
vulnerability can compromise both the host and any chats the bot has joined.
Please read this policy before reporting a security issue.

## Supported versions

We provide security fixes for the **latest minor release** on the `main`
branch. Older minor releases receive fixes only when an embargo requires it.

| Version  | Supported          |
| -------- | ------------------ |
| 1.0.x    | :white_check_mark: |
| < 1.0    | :x:                |

If you are running an unsupported release, your first remediation step is to
upgrade.

## Reporting a vulnerability

**Do not** open a public GitHub issue, pull request, or discussion for
security reports.

Instead, open a **private security advisory** through GitHub at
<https://github.com/staneswilson/cpp-mirror-leech-bot/security/advisories/new>.
Include:

-   A clear description of the issue and its impact.
-   Steps to reproduce, ideally with a minimal proof-of-concept.
-   The affected version (`cmlb --version` or the commit SHA).
-   Your preferred contact for follow-up, and whether you want public credit.

If you do not receive an acknowledgement within **five business days**, please
re-send with `[RESEND]` in the subject. We will respond with:

1.  An acknowledgement of the report.
2.  A tracking identifier you can reference in follow-up correspondence.
3.  An initial assessment of severity (CVSS v3.1 base score) within ten
    business days.
4.  A target remediation window proportional to severity.

## Disclosure policy

CMLB practises **coordinated disclosure**:

-   We aim to ship a fix within **90 days** of the initial report.
-   We will keep you updated at least every fourteen days while the issue is
    open.
-   Once a fix has shipped (or 90 days have elapsed, whichever comes first),
    we will publish a security advisory through GitHub Security Advisories and
    credit reporters who opted in.
-   If a fix requires longer than 90 days due to coordinating upstream
    dependencies (TDLib, Boost, OpenSSL), we will tell you and agree a revised
    timeline before extending the embargo.

## Scope

In scope:

-   The CMLB application binary and its source tree.
-   First-party adapters under `src/infrastructure/` and
    `include/cmlb/infrastructure/`.
-   The default configuration files and example deployment manifests.

Out of scope:

-   Issues in third-party dependencies (please report upstream and copy us).
-   Misconfigurations of the host operating system or container runtime.
-   Social-engineering or phishing reports unrelated to the codebase.
-   DoS reports that require unauthenticated, sustained traffic against
    deployments not controlled by the project.

## Hardening guidelines for operators

These are not optional for production deployments:

-   **No secrets in source.** Never commit `config.json`, `.env`,
    `service_account*.json`, or any file containing a Telegram bot token,
    API ID/hash, or third-party credential. These paths are listed in
    `.gitignore` and CI scans flag accidental additions.
-   **Use environment variables or sealed config files.** The recommended
    deployment pattern is to read secrets from environment variables or from
    a config file owned by a non-root service account with `0600`
    permissions. See `docs/runbook.md` for the full deployment recipe.
-   **Rotate credentials regularly.** Telegram bot tokens, qBittorrent
    Web UI passwords, Google Drive service-account keys, and aria2 RPC
    secrets must be rotated on a fixed cadence (we suggest 90 days) and
    immediately after any suspected compromise.
-   **Telegram credentials warrant special care.** A leaked bot token grants
    the holder full control over the bot identity, including reading
    messages from every chat the bot has joined. Treat it like a root SSH
    key: never log it, never paste it into chat, redact it in error
    reports, and revoke it through @BotFather the moment you suspect
    exposure.
-   **Restrict the bot's chats.** Configure the allowlist in `config.json`
    so the bot only responds to chats and users you control. The default
    configuration ships with an empty allowlist (deny-all) for this reason.
-   **Run with least privilege.** Do not run the bot as root. The bot does
    not need write access outside its data, downloads, and logs
    directories. Container deployments should drop all Linux capabilities.
-   **Monitor logs.** spdlog output should be shipped to a central log
    aggregator with PII / token redaction. Sample redaction rules are in
    `docs/runbook.md`.

## Cryptographic primitives

-   Transport security is provided by OpenSSL via Boost.Beast.
-   TDLib provides end-to-end encryption for Telegram traffic.
-   We do **not** roll our own cryptography. Patches that introduce custom
    crypto will be rejected.

## Verifying releases

Release artefacts are signed and accompanied by an SBOM. Verification
instructions are in [`docs/runbook.md`](./docs/runbook.md) under the
"Verifying releases" section.

## Acknowledgements

We thank the security researchers who have responsibly disclosed
vulnerabilities in CMLB. Credits are listed in the relevant security
advisory once the embargo lifts.

# CMLB Documentation

Start with the guide that matches your role:

| Need | Read |
|---|---|
| First deployment from an empty Linux machine | [`deployment_quickstart.md`](deployment_quickstart.md) |
| Production operations, backups, upgrades, troubleshooting | [`runbook.md`](runbook.md) |
| Every config field and environment override | [`configuration_reference.md`](configuration_reference.md) |
| Every Telegram command and permission tier | [`command_reference.md`](command_reference.md) |
| Architecture and dependency boundaries | [`architecture.md`](architecture.md) |
| Throughput measurement template | [`throughput_benchmarks.md`](throughput_benchmarks.md) |
| GitHub Wiki page map and publishing notes | [`wiki.md`](wiki.md) |
| Long-term technical decisions | [`adr/`](adr/) |

Recommended first-time path:

1. Use [`deployment_quickstart.md`](deployment_quickstart.md) to create the
   Telegram app, BotFather bot, command menu, `.env`, and Docker stack.
2. Use [`configuration_reference.md`](configuration_reference.md) only when you
   need to change a field beyond the default deployment.
3. Use [`runbook.md`](runbook.md) before putting the bot into a long-running
   production host.

The repository documentation is the source of truth. The GitHub Wiki, when
available, should use the concise page map in [`wiki.md`](wiki.md) and link
back here for detailed operator procedures.

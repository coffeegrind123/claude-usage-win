# Claude Usage for Windows

<img src="docs/icon.png" width="96" align="right">

Two rings in the Windows tray that tell you whether your AI coding subscriptions will last until their reset, and what to change if they won't. Claude Code, Codex, Cursor, Antigravity, Gemini CLI and Copilot: every account you're logged into, side by side.

Native C++, one self-contained exe with no runtime. The tray process idles at under 2 MB.

**The rings** show the session and the week of whichever tab is open (at first, the Claude login you used last). Each ring fills to its **speed**: the projected % at reset if you keep going like this. 100 means you run out exactly at reset. Green is ≤ 80, amber ≤ 100, and past 100 the ring becomes a solid red disc. A tab you aren't logged into has no speed, so its rings show what was **used**, on a tighter scale.

**Click** the icon to open the card; it closes when you click away or press Esc. Hovering shows what the rings mean. On the card:

- Click a tab to see that account. The tray icon follows it.
- Click anywhere else to sample now.
- Right-click the icon for: Sample now, Reminders, Claude homes, Open data folder, Start with Windows, Quit.

## What it shows

- **Speed, not percent.** "16% used" sounds fine until you notice it's 10% into the week.
- **One sentence of advice** from your actual model mix: stop or reduce Fable, move X% of Opus work to Sonnet, or do less. When you're fine it says "On pace — nothing to change".
- **Per-model weights measured on your own account.** The official usage % is joined with your local transcripts, and the weights (and whether cache reads are discounted) are fitted from that data. Until there's enough data it uses API price ratios.
- **API-equivalent cost** over a rolling day, week and 30 days.
- **Notifications** when the pace crosses 100, when it comes back, and when a window passes 90% (once per reset cycle).
- **Every account, remembered.** An account you aren't logged into shows its last reading and when it was taken. When its reset passes, that window drops to 0 and its ring turns green, which is the cue to switch back. Accounts unseen for a week are forgotten.
- **It samples again right after a reset is due**, instead of waiting for the next 10-minute tick.

## Install

Download `claude-usage-win-<version>-x64.zip` (or `-ARM64`) from [Releases](../../releases), unzip it anywhere, and run `ClaudeUsage.exe`. To keep it running, right-click the icon and choose **Start with Windows**.

The zip contains:

| | |
|---|---|
| `ClaudeUsage.exe` | the tray app; it also runs the sampler, every 10 minutes and on demand |
| `claude-usage.exe` | the same commands as a console program (`claude-usage status`, `claude-usage remind …`), to put on PATH |

Requires Windows 10 1809 or later (x64 or ARM64). The binaries are unsigned, so SmartScreen may ask once: **More info → Run anyway**.

## Claude Code in Docker / WSL2

Claude Usage reads files, not processes. When Claude Code runs in a container (for example under Docker Desktop on WSL2), its login and transcripts live in the container's home, and Windows can read that home through the bind mount. If your compose file has `C:\users\me\data:/home/me`, the files it needs are at:

| | in the container | on Windows |
|---|---|---|
| Claude login | `~/.claude/.credentials.json` | `C:\users\me\data\.claude\.credentials.json` |
| transcripts | `~/.claude/projects/` | `C:\users\me\data\.claude\projects\` |
| Codex | `~/.codex/` | `C:\users\me\data\.codex\` |
| gh (Copilot) | `~/.config/gh/hosts.yml` | `C:\users\me\data\.config\gh\hosts.yml` |

Right-click the icon, choose **Claude homes → Add home folder…**, and pick `C:\users\me\data`. A home inside a WSL distro works too, e.g. `\\wsl.localhost\Ubuntu\home\me`.

- **Every home is read.** Windows-native Claude Code (`%USERPROFILE%`) and the container's are shown side by side. Accounts are keyed by account id and transcript tokens by request id, so a login reachable from two homes is counted once.
- **State lives in the first home**, at `<home>\.claude\claude-usage\`. With the container's home first, `status.json` is at `~/.claude/claude-usage/status.json` inside the container, where a statusline or script can read it. When you add a home, the existing history is copied into the new state folder.
- **Tokens are never refreshed here.** Claude Code keeps `.credentials.json` fresh while it runs. While no session runs, the token can expire, and the card shows the last reading with "last checked".

From the command line: `claude-usage --home C:\users\me\data status`, or set `CLAUDE_USAGE_HOMES=C:\users\me\data;%USERPROFILE%`. `CLAUDE_USAGE_HOMES` replaces the list from the tray.

## Reminders

A reminder to start a Claude Code session later. Set it now, and paste the command when you're ready:

```
claude-usage remind add 2026-10-13T09:00 "Ads check" "check the ads spend and promo credit" --dir C:\code\site --flags --chrome
claude-usage remind add 2026-10-13T09:00 "Weekly review" "summarise last week's commits" --every week --dir /home/me/code
claude-usage remind list
claude-usage remind done <id>
```

When a reminder falls due, a notification is posted. Clicking it copies the command to the clipboard; nothing runs on its own. The form of the command depends on the reminder's directory:

- **A Windows path** gets `Set-Location -LiteralPath '<dir>'; claude <flags> '<prompt>'`. This works in Windows PowerShell 5.1, which has no `&&`, and in PowerShell 7.
- **A POSIX path** (a directory inside your container) gets `cd '<dir>' && claude <flags> '<prompt>'`, ready for a shell in the container.

`--every day|week|month` makes a reminder recurring: when it fires it moves to its next date, skipping any it missed while the machine was off. **Done** removes it for good. Due and upcoming reminders are also listed on the card and in the right-click menu, each with **Copy** and **Done**.

Reminders are kept in `<state>\reminders.json`.

## For other tools

`<state>\status.json` is rewritten every 10 minutes:

- **Top level, for the default Claude Code login** (the primary home's; `acct` names it): `pace`, `level`, `advice`, `windows[]` (used %, speed, projected, hours left), `cost` (rolling `day`/`week`/`month` over every home's transcripts) and `fit`.
- **`accounts[]`**: the same windows for every account, each with its own `pace`/`level`/`advice` and, for Claude, `dir`.
- **Errors**: when the Claude read fails, `error` and `errorAt` are set and `t` is left alone. Check `t` for freshness before trusting the top-level figures.

`claude-usage status` prints the same.

## How the number is made

- Week windows: `projected = used% / fraction of the window elapsed`.
- The 5-hour session: the rate over the last 45 minutes, projected to the reset. A session is bursty, so an average since it opened lags.
- The headline is the worst window.

## Providers

| | read from | windows |
|---|---|---|
| Claude Code | `.claude\.credentials.json` in every home, plus every `CLAUDE_CONFIG_DIR` profile (`.claude-*`) in them | session, week, per-model week |
| Codex | `.codex\auth.json`, live (and `CODEX_HOME`); its session transcripts as the fallback | 5 h, week |
| Antigravity | the running app's local language server, one tab per model vendor it resells | per-model allowance |
| Cursor | the IDE's login (`%APPDATA%\Cursor\…\state.vscdb`, read with Windows' built-in SQLite) | billing month (paid seats) |
| Gemini CLI, GitHub Copilot | their stored logins (`.gemini\oauth_creds.json`; `gh auth token` or `hosts.yml`). **Untested**: written from their public clients | daily per model · monthly premium requests |

A provider with no login on the machine costs nothing and shows nothing. Claude Usage only ever talks to a provider's own usage endpoint, with the login that provider already stored, and it never refreshes or writes a token. Per-model weights, the advice from your model mix and the API-equivalent cost are Claude-only.

## Build

You need Visual Studio 2022 or later (or its Build Tools) with the C++ workload, and CMake 3.21+:

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

CI builds x64 and ARM64 on every push and attaches both zips to a release for every `v*` tag.

## How it is put together

| | |
|---|---|
| `src/pace.*` | the pace model: windows, verdict, advice, NNLS fit, cost, accounts |
| `src/providers.*` | Claude, Codex, Cursor, Gemini, Copilot, Antigravity |
| `src/http.*` | WinHTTP plus the retry policy: one 20 s read budget per tick, with each attempt clamped to it |
| `src/cli.*` | `tick`, `status`, `fit`, `remind` |
| `src/app.cpp`, `card.*`, `draw.*` | the tray, the card (GDI+), notifications, the 10-minute scheduler |
| `src/json.*`, `util.*`, `config.*` | the JSON DOM, Win32 helpers, homes and paths |

The tray never samples in-process. It runs its own exe as `ClaudeUsage.exe tick` in a hidden child process every 10 minutes (and on a click, after a reset, after a failed read, and on resume from sleep), and then reads the `status.json` that child writes. That keeps the work of reading every sample and transcript out of the long-running process. The child's output goes to `<state>\tick.log`.

## License

MIT; see [LICENSE](LICENSE).

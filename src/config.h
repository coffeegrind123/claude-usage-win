#pragma once
// Where Claude Usage looks, and where it keeps its state.
//
// A "home" is a directory laid out like a Unix $HOME: `.claude\`, `.claude.json`, `.claude-*\`, `.codex\`,
// `.gemini\`, `.config\gh\`. Windows-native Claude Code uses %USERPROFILE%. Claude Code in a container keeps
// the same layout in the container's home, which Windows sees through the bind mount — e.g. Docker Desktop on
// WSL2 bind-mounting C:\users\me\data as /home/me. Every configured home is read, so both kinds of login
// show up side by side; accounts are keyed by id and transcript tokens by request id, so a home seen twice is
// never counted twice.
//
//   order   source
//   1       --home <dir> on the command line (repeatable)
//   2       CLAUDE_USAGE_HOMES=<dir>;<dir>   — when set, it is the whole list (tests, scripted setups)
//   2'      otherwise: the homes added in the tray (HKCU\Software\ClaudeUsageWin, "Homes"), then %USERPROFILE%
//
// State (status.json, samples.jsonl, accounts.json, reminders.json) lives in <first home>\.claude\claude-usage.
// With a container's home first, tools inside the container read the same status.json.
#include <string>
#include <vector>

namespace config {

constexpr const wchar_t* REG_KEY = L"Software\\ClaudeUsageWin";

/// Prepended to the list; call before the first `homes()`.
void addHomeOverride(const std::wstring& dir);
/// Existing directories, normalised and de-duplicated, in priority order.
const std::vector<std::wstring>& homes();
/// The homes added in the tray (stored, whether or not they exist right now).
std::vector<std::wstring> storedHomes();
void setStoredHomes(const std::vector<std::wstring>& dirs);
/// True when CLAUDE_USAGE_HOMES decides the list, so the tray's own list does not apply.
bool homesFromEnv();

std::wstring stateDir();
std::wstring samplesPath();
std::wstring statusPath();
std::wstring accountsPath();
std::wstring remindersPath();
std::wstring tickLogPath();

}  // namespace config

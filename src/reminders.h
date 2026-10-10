#pragma once
// Reminders to start a Claude Code session later: `claude-usage remind add` writes them, the tray notifies when one
// falls due, and a click copies the command to paste when ready. Nothing runs on its own. Kept in
// reminders.json in the state directory, so anything that writes that file shares the list.
#include <optional>
#include <string>
#include <vector>

#include "json.h"

namespace reminders {

struct Reminder {
    std::string id;
    double at = 0;
    std::string title, prompt, dir, flags;
    std::string every;   // "", "day", "week" or "month"
};

/// The raw list, in file order. Nothing (an empty array) when the file is absent; nullopt when it is unreadable JSON.
std::optional<json::Value> loadRaw();
bool saveRaw(const json::Value& list);
/// The valid entries; one missing its id, time, title or prompt is skipped.
std::vector<Reminder> load();
std::optional<Reminder> fromJson(const json::Value& r);

/// What a click copies. A POSIX `dir` (a path inside a container) gets `cd '<dir>' && claude …` for a shell
/// there; a Windows `dir` gets `Set-Location -LiteralPath '<dir>'; claude …`, which works in Windows
/// PowerShell 5.1 (no `&&`) as well as PowerShell 7.
std::string command(const Reminder& r);

/// A recurring reminder's next date after `nowMs`, in local calendar terms: a day keeps its wall-clock time
/// across DST, a month clamps the 31st to the month's last day. Occurrences missed while the machine slept
/// are skipped, not replayed. nullopt for an unknown unit.
std::optional<double> nextDate(double atMs, const std::string& every, double nowMs);

// Each rewrites one reminder in place; the CLI is the file's other writer.
void remove(const std::string& id);
void reschedule(const std::string& id, double at);

}  // namespace reminders

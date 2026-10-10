#pragma once
// What the tray reads: status.json (written by `tick`), shaped for drawing,
// including the rule that rolls a window over the minute its reset passes.
#include <optional>
#include <string>
#include <vector>

namespace ui {

struct Win {
    std::string key;
    double percent = 0, projected = 0, speed = 0, allowed = 0, rate = 0;
    std::optional<double> hoursLeft;
    bool isLong = false;
};

/// One subscription seen on this machine. `current` = the login its provider is using right now; the rest
/// are remembered readings, already rolled past any reset by the tick.
struct Account {
    std::string id, provider, vendor;
    std::optional<std::string> title, via, email, plan, advice;
    bool current = false;
    double asOf = 0;
    std::vector<Win> windows;
};

struct Status {
    std::optional<double> pace;
    std::string level = "unknown", worst;
    double t = 0;
    std::optional<std::string> error;
    std::vector<Win> windows;
    std::vector<Account> accounts;
    std::optional<std::string> acct;   // the login the TOP LEVEL is pinned to — not necessarily accounts[0], the one used last
    double stamp = 0;                  // moves on EVERY tick, failed ones included (`t` deliberately does not)
    double accountsT = 0;              // when `accounts` were read; differs from `t` only while the Claude read fails
    std::optional<double> nextReset;   // epoch seconds of the soonest reset among live logins
    std::optional<std::string> advice;
    double costDay = 0, costWeek = 0, costMonth = 0;
    bool hasCost = false;
};

Status loadStatus();
/// Identifies a reset CYCLE by the absolute instant it ends — read time plus what is left — so it is constant
/// within a cycle and jumps by a whole window at a turnover. Never derive this from a countdown: `hoursLeft`
/// shrinks between reads, which would let the 90% warning re-arm every few hours.
long long cycleId(double readAtMs, double hoursLeft);
/// "2d 4h", "3h 12m", "45m".
std::string countdown(double hours);
const char* providerName(const std::string& provider);

}  // namespace ui

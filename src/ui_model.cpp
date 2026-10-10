#include "ui_model.h"

#include <algorithm>
#include <cmath>

#include "config.h"
#include "json.h"
#include "util.h"

namespace ui {

namespace {

std::optional<std::string> optStr(const json::Value& v) {
    return v.isStr() ? std::optional<std::string>(v.str()) : std::nullopt;
}

Win window(const json::Value& w) {
    Win x;
    x.key = w["key"].str();
    x.percent = w["percent"].num(0);
    x.projected = w["projected"].num(0);
    x.speed = w["speed"].num(0);
    x.allowed = w["allowedPerHour"].num(0);
    if (w["minutesLeft"].isNum()) {
        x.hoursLeft = w["minutesLeft"].num() / 60;
    }
    x.isLong = w["long"].boolean(false);
    x.rate = w["rate"].num(0);
    return x;
}

}  // namespace

Status loadStatus() {
    Status s;
    std::string text;
    json::Value j;
    if (!util::readFile(config::statusPath(), text) || !json::parse(text, j) || !j.isObj()) {
        // The first launch, before the first tick lands: still a card, saying what is happening.
        s.error = "no status.json yet — sampling…";
        Account a;
        a.provider = "claude";
        a.vendor = "claude";
        a.current = true;
        a.asOf = util::nowMs();
        a.advice = "waiting for the first sample";
        s.accounts.push_back(std::move(a));
        return s;
    }
    if (j["pace"].isNum()) {
        s.pace = j["pace"].num();
    }
    s.level = j["level"].str("unknown");
    s.worst = j["worst"].str();
    s.t = j["t"].num(0);
    s.error = optStr(j["error"]);
    s.acct = optStr(j["acct"]);
    s.advice = optStr(j["advice"]);
    if (j["cost"].isObj()) {
        s.costDay = j["cost"]["day"]["total"].num(0);
        s.costWeek = j["cost"]["week"]["total"].num(0);
        s.costMonth = j["cost"]["month"]["total"].num(0);
        s.hasCost = true;
    }
    for (const auto& w : j["windows"].items()) {
        s.windows.push_back(window(w));
    }
    for (const auto& a : j["accounts"].items()) {
        Account x;
        x.id = a["id"].str();
        x.provider = a["provider"].str("claude");
        x.vendor = a["vendor"].isStr() ? a["vendor"].str() : x.provider;
        x.title = optStr(a["title"]);
        x.via = optStr(a["via"]);
        x.email = optStr(a["email"]);
        x.plan = optStr(a["plan"]);
        x.current = a["current"].boolean(false);
        x.asOf = a["asOf"].num(s.t);
        for (const auto& w : a["windows"].items()) {
            x.windows.push_back(window(w));
        }
        x.advice = optStr(a["advice"]);
        s.accounts.push_back(std::move(x));
    }
    // A reset that has passed since the tick is a fact the file cannot know yet: zero that window here, so the
    // rings and rows turn over at the minute it happens, not at the next sample.
    s.accountsT = j["accountsAt"].num(s.t);
    s.stamp = std::max({s.t, s.accountsT, j["errorAt"].num(0)});
    double nowMs = util::nowMs();
    // Keep in step with `pace::rolled`, which applies the same rule at tick time.
    auto rolled = [&](std::vector<Win>& ws, double readAt) {
        for (Win& w : ws) {
            if (w.hoursLeft && *w.hoursLeft - (nowMs - readAt) / 3600000 <= 0) {
                w.percent = 0;
                w.projected = 0;
                w.speed = 0;
                w.rate = 0;
                w.hoursLeft.reset();
            }
        }
    };
    std::vector<double> resets;
    for (const Win& w : s.windows) {
        if (w.hoursLeft) {
            resets.push_back(s.t / 1000 + *w.hoursLeft * 3600);
        }
    }
    for (const Account& a : s.accounts) {
        if (!a.current) {
            continue;
        }
        for (const Win& w : a.windows) {
            if (w.hoursLeft) {
                resets.push_back(s.accountsT / 1000 + *w.hoursLeft * 3600);
            }
        }
    }
    // Every hoursLeft is ahead of the read that wrote it, so a reset that has passed SINCE stays the target until
    // a newer read lands — dropping it the moment it passes would leave the reset poll nothing to aim at.
    if (!resets.empty()) {
        s.nextReset = std::round(*std::min_element(resets.begin(), resets.end()));
    }
    rolled(s.windows, s.t);
    for (Account& a : s.accounts) {
        rolled(a.windows, s.accountsT);
    }
    // A status.json written before accounts existed still draws: it is one account, the live one.
    if (s.accounts.empty()) {
        Account a;
        a.provider = "claude";
        a.vendor = "claude";
        a.current = true;
        a.asOf = s.t;
        a.windows = s.windows;
        a.advice = s.advice;
        s.accounts.push_back(std::move(a));
    }
    return s;
}

long long cycleId(double readAtMs, double hoursLeft) {
    return static_cast<long long>(readAtMs / 1000 + hoursLeft * 3600);
}

std::string countdown(double hours) {
    long long m = static_cast<long long>(std::round(hours * 60));
    long long d = m / 1440, h = (m % 1440) / 60, mm = m % 60;
    if (d > 0) {
        return std::to_string(d) + "d " + std::to_string(h) + "h";
    }
    if (h > 0) {
        return std::to_string(h) + "h " + std::to_string(mm) + "m";
    }
    return std::to_string(mm) + "m";
}

const char* providerName(const std::string& p) {
    if (p == "claude") return "Claude";
    if (p == "codex") return "Codex";
    if (p == "cursor") return "Cursor";
    if (p == "gemini") return "Gemini";
    if (p == "antigravity") return "Antigravity";
    if (p == "copilot") return "Copilot";
    return nullptr;
}

}  // namespace ui

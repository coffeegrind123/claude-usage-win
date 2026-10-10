#include "reminders.h"

#include <cmath>
#include <ctime>

#include "config.h"
#include "util.h"

namespace reminders {

std::optional<json::Value> loadRaw() {
    std::string text;
    if (!util::readFile(config::remindersPath(), text)) {
        return json::Value::array();
    }
    json::Value j;
    if (!json::parse(text, j)) {
        return std::nullopt;
    }
    return j;
}

bool saveRaw(const json::Value& list) {
    util::makeDirs(config::stateDir());
    return util::writeFileAtomic(config::remindersPath(), json::dump(list, 2));
}

std::optional<Reminder> fromJson(const json::Value& r) {
    if (!r["id"].isStr() || !r["at"].isNum() || !r["title"].isStr() || !r["prompt"].isStr()) {
        return std::nullopt;
    }
    Reminder x;
    x.id = r["id"].str();
    x.at = r["at"].num();
    x.title = r["title"].str();
    x.prompt = r["prompt"].str();
    x.dir = r["dir"].isStr() ? r["dir"].str() : util::utf8(util::homeDir());
    x.flags = r["flags"].str();
    x.every = r["every"].str();
    return x;
}

std::vector<Reminder> load() {
    std::vector<Reminder> out;
    std::optional<json::Value> raw = loadRaw();
    if (!raw) {
        return out;
    }
    for (const auto& r : raw->items()) {
        if (auto x = fromJson(r)) {
            out.push_back(*x);
        }
    }
    return out;
}

std::string command(const Reminder& r) {
    bool posix = !r.dir.empty() && (r.dir[0] == '/' || r.dir[0] == '~');
    std::string flags = r.flags.empty() ? "" : r.flags + " ";
    if (posix) {
        auto q = [](const std::string& x) { return "'" + util::replaceAll(x, "'", "'\\''") + "'"; };
        return "cd " + q(r.dir) + " && claude " + flags + q(r.prompt);
    }
    // PowerShell ends a single-quoted string at any of its four single-quote characters, the typographic
    // ones included — each is escaped by doubling it.
    auto q = [](std::string x) {
        for (const char* quote : {"'", "\xE2\x80\x98", "\xE2\x80\x99", "\xE2\x80\x9A", "\xE2\x80\x9B"}) {
            x = util::replaceAll(x, quote, std::string(quote) + quote);
        }
        return "'" + x + "'";
    };
    return "Set-Location -LiteralPath " + q(r.dir) + "; claude " + flags + q(r.prompt);
}

namespace {

int daysIn(int year, int month0) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month0 == 1 && leap ? 29 : days[month0];
}

/// `Calendar.date(byAdding:value:to:)` over local time.
std::optional<double> add(double startMs, const std::string& every, int n) {
    double whole = std::floor(startMs / 1000);
    double frac = startMs - whole * 1000;
    time_t t = static_cast<time_t>(whole);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) {
        return std::nullopt;
    }
    if (every == "day") {
        tm.tm_mday += n;
    } else if (every == "week") {
        tm.tm_mday += 7 * n;
    } else {
        int months = tm.tm_mon + n;
        int year = tm.tm_year + 1900 + months / 12;
        int month0 = months % 12;
        tm.tm_year = year - 1900;
        tm.tm_mon = month0;
        if (tm.tm_mday > daysIn(year, month0)) {
            tm.tm_mday = daysIn(year, month0);
        }
    }
    tm.tm_isdst = -1;
    time_t out = mktime(&tm);
    if (out == static_cast<time_t>(-1)) {
        return std::nullopt;
    }
    return static_cast<double>(out) * 1000 + frac;
}

}  // namespace

std::optional<double> nextDate(double atMs, const std::string& every, double nowMs) {
    if (every != "day" && every != "week" && every != "month") {
        return std::nullopt;
    }
    double d = atMs;
    // Always from the original date, never step on step: Jan 31 + 1 month is Feb 28, and + 2 is Mar 31.
    for (int n = 1; d <= nowMs; n++) {
        std::optional<double> x = add(atMs, every, n);
        if (!x) {
            return std::nullopt;
        }
        d = *x;
    }
    return d;
}

namespace {
template <class F>
void rewrite(const std::string& id, F change) {
    std::optional<json::Value> raw = loadRaw();
    if (!raw || !raw->isArr()) {
        return;
    }
    json::Value out = json::Value::array();
    for (const auto& r : raw->items()) {
        if (r["id"].str() != id) {
            out.push(r);
            continue;
        }
        if (std::optional<json::Value> x = change(r)) {
            out.push(*x);
        }
    }
    saveRaw(out);
}
}  // namespace

void remove(const std::string& id) {
    rewrite(id, [](const json::Value&) -> std::optional<json::Value> { return std::nullopt; });
}

void reschedule(const std::string& id, double at) {
    rewrite(id, [at](const json::Value& r) -> std::optional<json::Value> {
        json::Value x = r;
        x.set("at", at);
        return x;
    });
}

}  // namespace reminders

// claude-usage tick     sample the usage APIs + local transcripts, append to samples.jsonl, write status.json
// claude-usage status   print the current status (and refresh it from stored samples only — no network)
// claude-usage fit      show the fitted per-model weights and the data behind them
// claude-usage remind add <when> <title> <prompt> [--dir <path>] [--flags '<claude flags>'] [--every day|week|month]
// claude-usage remind list | done <id>
//
// Two data sources, joined: the usage endpoint (server truth, percent per window, no breakdown) and the
// transcripts under .claude\projects (every assistant message with model + token usage). Each tick records
// both; over days a least-squares fit of Δpercent against tokens-by-model gives MEASURED weights.
#include "cli.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <random>
#include <stdexcept>

#include "config.h"
#include "http.h"
#include "json.h"
#include "pace.h"
#include "providers.h"
#include "reminders.h"
#include "util.h"
#include "version.h"

namespace cli {

namespace {

using util::jsNum;
using util::jsRound;

void say(const std::string& s) { util::out(s + "\n"); }
void warn(const std::string& s) { util::err(s + "\n"); }

std::vector<pace::Sample> loadSamples() {
    std::vector<pace::Sample> out;
    std::string text;
    if (!util::readFile(config::samplesPath(), text)) {
        return out;
    }
    for (std::string_view line : util::splitLines(text)) {
        if (line.empty()) {
            continue;
        }
        json::Value j;
        pace::Sample s;
        if (json::parse(line, j) && pace::sampleFromJson(j, s)) {
            out.push_back(std::move(s));
        }
    }
    return out;
}

pace::Samples view(const std::vector<pace::Sample>& all) {
    pace::Samples v;
    v.reserve(all.size());
    for (const auto& s : all) {
        v.push_back(&s);
    }
    return v;
}

template <class Pred>
pace::Samples filter(const pace::Samples& all, Pred keep) {
    pace::Samples v;
    for (const pace::Sample* s : all) {
        if (keep(*s)) {
            v.push_back(s);
        }
    }
    return v;
}

json::Value loadAccounts() {
    std::string text;
    json::Value j;
    if (util::readFile(config::accountsPath(), text) && json::parse(text, j) && j.isObj()) {
        return j;
    }
    return json::Value::object();
}

bool writeStatus(const json::Value& s) {
    util::makeDirs(config::stateDir());
    return util::writeFileAtomic(config::statusPath(), json::dump(s, 2));
}

std::string upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return s;
}

std::string str(const json::Value& v) {
    if (v.isNull()) return "null";
    if (v.isNum()) return jsNum(v.num());
    if (v.isBool()) return v.boolean() ? "true" : "false";
    return v.str();
}

void printStatus(const json::Value& s) {
    say("PACE " + str(s["pace"]) + " (" + upper(s["level"].str()) + ", worst: " + str(s["worst"]) + ")  — " + util::isoString(s["t"].num()));
    say("  " + (s["advice"].truthy() ? s["advice"].str() : std::string("on pace — nothing to change")));
    std::string byModel;
    for (const auto& [m, v] : s["cost"]["week"]["byModel"].members()) {
        byModel += (byModel.empty() ? "" : ", ") + m + " $" + jsNum(v.num());
    }
    say("  API-equivalent cost (.claude\\projects): day $" + jsNum(s["cost"]["day"]["total"].num()) + " · week $" + jsNum(s["cost"]["week"]["total"].num()) +
        " · month $" + jsNum(s["cost"]["month"]["total"].num()) + " (week: " + byModel + ")");
    for (const auto& w : s["windows"].items()) {
        std::string head = "  " + util::padEnd(w["key"].str(), 20) + " " + util::padStart(jsNum(w["percent"].num()), 3) + "%";
        if (w["idle"].truthy()) {   // no reset yet: nothing to project
            say(head + " · idle");
            continue;
        }
        std::string hoursLeft = jsNum(jsRound(w["minutesLeft"].num() / 60));
        if (w["long"].truthy()) {
            say(head + " at " + jsNum(jsRound(w["elapsed"].num() * 100)) + "% of window · " + util::fixed(w["speed"].num(), 2) + "%/h avg → " +
                jsNum(jsRound(w["projected"].num())) + "% at reset · may spend " + util::fixed(w["allowedPerHour"].num(), 2) + "%/h to hit " +
                jsNum(s["target"].num()) + " · " + hoursLeft + "h left");
        } else {
            say(head + " · " + util::fixed(w["rate"].num(), 1) + "%/h last " + jsNum(pace::RATE_WINDOW_MIN) + "m → " + jsNum(jsRound(w["projected"].num())) +
                "% at reset · " + hoursLeft + "h left");
        }
    }
    std::string spend;
    for (const auto& [m, t] : s["spend24h"].members()) {
        spend += (spend.empty() ? "" : " · ") + m + " in " + util::fixed(t["input"].num() / 1e6, 1) + " out " + util::fixed(t["output"].num() / 1e6, 2) + " cw " +
                 util::fixed(t["cache_write"].num() / 1e6, 1) + " cr " + util::fixed(t["cache_read"].num() / 1e6, 0) + " (" + jsNum(t["requests"].num()) + " req)";
    }
    if (!spend.empty()) {
        say("  last 24h tokens (M): " + spend);
    }
    const json::Value& f = s["fit"];
    if (f["enough"].truthy()) {
        std::string rel;
        for (const auto& [m, r] : f["relative"].members()) {
            rel += (rel.empty() ? "" : ", ") + m + " " + util::fixed(r.num(), 1) + "×";
        }
        say("  fitted weight vs sonnet: " + rel + "  (" + jsNum(f["n"].num()) + " samples, residual " + util::fixed(f["residual"].num(), 2) + ")");
        const json::Value& fc = s["fitCacheFull"];
        if (fc.truthy()) {
            say(std::string("  cache reads: ") + (fc["residual"].num() < f["residual"].num() ? "fit better at FULL price" : "fit better DISCOUNTED (0.1×)") +
                " (residual " + util::fixed(fc["residual"].num(), 2) + " vs " + util::fixed(f["residual"].num(), 2) + ")");
        }
    } else if (f.truthy()) {
        say("  fit: " + jsNum(f["n"].num()) + " usable samples so far, need ~" + std::to_string(f["models"].size() * 3) + " with a mix of models");
    }
    for (const auto& [k, ts] : s["resets"].members()) {
        std::string list;
        for (const auto& t : ts.items()) {
            list += (list.empty() ? "" : ", ") + util::isoString(t.num()).substr(0, 16);
        }
        say("  observed resets " + k + ": " + list);
    }
}

// ---------------------------------------------------------------- tick
struct Login {
    providers::Profile profile;
    std::vector<pace::Limit> limits;
    providers::Identity acct;
    double used = 0;
};

/// One tick at a time per state directory: the tray's timer, a click and a hand-run `claude-usage tick` can overlap.
class TickLock {
public:
    TickLock() {
        unsigned long long h = 1469598103934665603ULL;   // FNV-1a over the lower-cased state path
        for (wchar_t c : util::wide(util::lower(util::utf8(config::stateDir())))) {
            h = (h ^ static_cast<unsigned long long>(c)) * 1099511628211ULL;
        }
        std::wstring name = L"Local\\ClaudeUsageWin.tick." + std::to_wstring(h);
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
    }
    ~TickLock() {
        if (owned_) {
            ReleaseMutex(mutex_);
        }
        if (mutex_) {
            CloseHandle(mutex_);
        }
    }
    TickLock(const TickLock&) = delete;
    TickLock& operator=(const TickLock&) = delete;

    /// False when another tick held it: that one's status.json is the fresh one, so this one has nothing to add.
    bool acquire() {
        if (!mutex_) {
            return true;
        }
        DWORD r = WaitForSingleObject(mutex_, 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
            owned_ = true;
            return true;
        }
        r = WaitForSingleObject(mutex_, 120000);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
            owned_ = true;
        }
        return false;
    }

private:
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
};

int tick() {
    TickLock lock;
    if (!lock.acquire()) {
        warn("another tick was running; its result stands");
        return 0;
    }
    double now = util::nowMs();
    http::startReadBudget(now);   // ONE deadline for every provider read this tick — caps the tick, not each read
    std::vector<pace::Sample> samples = loadSamples();
    double last = !samples.empty() && samples.back().t ? samples.back().t : now - 10 * 60000;

    // Every profile that answers is a live login. Nothing here refreshes a token (that is Claude Code's, and
    // rotating it from here could log a session out), so a profile that has not run for a while just keeps
    // its last reading.
    std::vector<Login> logins;
    std::string lastError = "no Claude Code login found";
    for (const auto& profile : providers::claudeProfiles()) {
        try {
            std::optional<providers::Credential> cred = providers::credential(profile);
            if (!cred) {
                continue;
            }
            Login l;
            l.profile = profile;
            l.limits = providers::fetchUsage(*cred);
            l.acct = providers::identity(*cred, profile);
            l.used = util::mtimeMs(profile.config);
            if (!std::isfinite(l.used)) {
                throw std::runtime_error("ENOENT: no such file, stat '" + util::utf8(profile.config) + "'");
            }
            logins.push_back(std::move(l));
        } catch (const std::exception& e) {
            lastError = e.what();
            warn(util::utf8(profile.isDefault() ? util::join(profile.home, L".claude") : profile.dir) + ": " + e.what());   // one dead profile must leave a trace
        }
    }
    // The same account reached through two profiles (or two homes) is one account: the default profile's entry
    // is the one kept, then the primary home's, then the fresher — and it is as recent as its freshest profile.
    std::map<std::string, double> usedLast;
    for (const auto& l : logins) {
        usedLast[l.acct.id] = std::max(usedLast[l.acct.id], l.used);
    }
    for (auto& l : logins) {
        l.used = usedLast[l.acct.id];
    }
    std::stable_sort(logins.begin(), logins.end(), [](const Login& a, const Login& b) {
        if (a.profile.isDefault() != b.profile.isDefault()) return a.profile.isDefault();
        if (a.profile.homeIndex != b.profile.homeIndex) return a.profile.homeIndex < b.profile.homeIndex;
        return a.used > b.used;
    });
    std::vector<Login> unique;
    for (auto& l : logins) {
        if (std::none_of(unique.begin(), unique.end(), [&](const Login& u) { return u.acct.id == l.acct.id; })) {
            unique.push_back(l);
        }
    }
    std::stable_sort(unique.begin(), unique.end(), [](const Login& a, const Login& b) { return a.used < b.used; });
    // Top-level status is a contract other tools read as "the default Claude login", so it is pinned to the
    // primary home's default profile; only when that one is unreadable does it fall to the login used last.
    const Login* recent = unique.empty() ? nullptr : &unique.back();
    const Login* head = nullptr;
    for (const auto& l : unique) {
        if (l.profile.isDefault() && (!head || l.profile.homeIndex < head->profile.homeIndex)) {
            head = &l;
        }
    }
    if (!head) {
        head = recent;
    }
    json::Value accounts = loadAccounts();

    // One sample per live login, the pinned one last (it is what `status` rebuilds from). Transcripts are shared
    // between profiles, so tokens cannot be split by account: they ride on the pinned sample only.
    if (head) {
        pace::Ordered<pace::Tokens> tokens = providers::scanTokens(last, now);
        std::vector<const Login*> order;
        for (const auto& l : unique) {
            if (&l != head) {
                order.push_back(&l);
            }
        }
        order.push_back(head);
        util::makeDirs(config::stateDir());
        std::string lines;
        for (const Login* l : order) {
            pace::Sample s;
            s.t = now;
            s.acct = l->acct.id;
            s.limits = l->limits;
            if (l == head) {
                s.tokens = tokens;
            }
            lines += json::dump(pace::toJson(s)) + "\n";
            samples.push_back(std::move(s));
            json::Value acct = json::Value::object();
            acct.set("id", l->acct.id);
            acct.set("email", l->acct.email);
            acct.set("plan", l->acct.plan);
            acct.set("provider", "claude");
            acct.set("limits", pace::toJson(l->limits));
            acct.set("dir", l->profile.dirJson());
            pace::rememberAccount(accounts, acct, now);
        }
        util::appendFile(config::samplesPath(), lines);
    }
    pace::Samples all = view(samples);
    pace::Live live;
    for (const auto& l : unique) {
        if (&l != head) {
            std::string id = l.acct.id;
            live[id] = filter(all, [&](const pace::Sample& s) { return s.acct == id; });
        }
    }
    std::deque<providers::Reading> held;   // `live` points into these readings
    std::vector<std::vector<providers::Reading> (*)(double)> reads;
    reads.push_back([](double n) { return providers::codexReadings(n, true); });
    for (auto p : providers::others()) {
        reads.push_back(p);
    }
    for (auto read : reads) {
        std::vector<providers::Reading> got;
        try {
            got = read(now);
        } catch (const std::exception& e) {
            warn(e.what());
            continue;
        }
        for (auto& x : got) {
            std::string id = x.acct["id"].str();
            pace::rememberAccount(accounts, x.acct, now);
            accounts.find(id)->set("asOf", x.asOf);
            held.push_back(std::move(x));
            live[id] = view(held.back().readings);
        }
    }
    util::makeDirs(config::stateDir());
    util::writeFileAtomic(config::accountsPath(), json::dump(accounts, 2));

    if (!head) {
        // No Claude login answered (tokens expire while Claude Code is not running). The other providers were
        // still read; the Claude figures keep their own time `t`, so nothing downstream mistakes them for fresh.
        json::Value status = json::Value::object();
        std::string text;
        if (util::readFile(config::statusPath(), text)) {
            json::Value j;
            if (json::parse(text, j) && j.isObj()) {
                status = std::move(j);
            }
        }
        status.set("error", "usage unreadable: " + lastError);
        status.set("errorAt", now);
        std::string firstId = status["accounts"][0]["id"].str();
        json::Value shown = pace::accountsView(accounts, "", {}, now, live, firstId);
        // With no Claude read there is no telling which Claude login is in use, so none of them is told to "switch back".
        for (auto& a : shown.items()) {
            if (a["provider"].str() == "claude") {
                a.set("advice", "last reading — this login could not be read just now");
            }
        }
        status.set("accounts", std::move(shown));
        status.set("accountsAt", now);
        writeStatus(status);
        warn(status["error"].str());
        return 1;
    }
    std::string headId = head->acct.id;
    json::Value status = pace::buildStatus(head->limits, all, now, headId);
    status.set("accounts", pace::accountsView(accounts, headId, filter(all, [&](const pace::Sample& s) { return s.acct.empty() || s.acct == headId; }), now, live,
                                              recent->acct.id));
    status.set("accountsAt", now);   // accounts are read even when the Claude figures could not be, so they carry their own time
    writeStatus(status);
    printStatus(status);
    return 0;
}

int status() {
    std::vector<pace::Sample> samples = loadSamples();
    if (samples.empty()) {
        say("no samples yet — run `claude-usage tick`");
        return 0;
    }
    pace::Samples all = view(samples);
    const pace::Sample& last = samples.back();
    json::Value s = pace::buildStatus(last.limits, all, last.t, last.acct);
    // No network here: Codex is read from its transcripts, every other provider from its remembered reading.
    std::vector<providers::Reading> codex = providers::codexReadings(last.t, false);
    json::Value accounts = loadAccounts();
    pace::Live live;
    for (const auto& c : codex) {
        live[c.acct["id"].str()] = view(c.readings);
    }
    // 30 min = the staleness line; the tray draws "last checked" past the same STALE_AFTER_MS.
    for (const auto& [k, a] : accounts.members()) {
        std::string id = a["id"].str();
        if (a["provider"].str() != "codex" && id != last.acct && a["asOf"].isNum() && last.t - a["asOf"].num() < pace::STALE_AFTER_MS) {
            live[id] = filter(all, [&](const pace::Sample& x) { return x.acct == id; });
        }
    }
    std::string acct = last.acct;
    s.set("accounts", pace::accountsView(accounts, acct, filter(all, [&](const pace::Sample& x) { return x.acct.empty() || x.acct == acct; }), last.t, live, acct));
    writeStatus(s);
    printStatus(s);
    return 0;
}

int fitCommand() {
    std::vector<pace::Sample> samples = loadSamples();
    pace::Samples all = view(samples);
    json::Value o = json::Value::object();
    o.set("prior", pace::toJson(pace::fit(all)));
    o.set("cacheFull", pace::toJson(pace::fit(all, 1)));
    say(json::dump(o, 2));
    return 0;
}

// ---------------------------------------------------------------- reminders
std::string randomId() {
    static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::string id;
    for (int i = 0; i < 6; i++) {
        id += digits[rd() % 36];
    }
    return id;
}

int remind(std::vector<std::string> args) {
    std::optional<json::Value> raw = reminders::loadRaw();
    if (!raw || !raw->isArr()) {
        warn("reminders.json is not a JSON array: " + util::utf8(config::remindersPath()));
        return 1;
    }
    json::Value list = *raw;
    std::string sub = args.empty() ? "" : args[0];
    std::vector<std::string> rest(args.begin() + (args.empty() ? 0 : 1), args.end());
    if (sub == "add") {
        auto opt = [&](const std::string& name, std::optional<std::string> dflt) -> std::optional<std::string> {
            auto it = std::find(rest.begin(), rest.end(), name);
            if (it == rest.end()) {
                return dflt;
            }
            std::optional<std::string> v;
            if (it + 1 != rest.end()) {
                v = *(it + 1);
                rest.erase(it, it + 2);
            } else {
                rest.erase(it);
            }
            return v;
        };
        std::optional<std::string> dir = opt("--dir", util::utf8(util::cwd()));
        std::optional<std::string> flags = opt("--flags", std::string());
        std::optional<std::string> every = opt("--every", std::nullopt);
        std::string when = rest.size() > 0 ? rest[0] : "", title = rest.size() > 1 ? rest[1] : "", prompt = rest.size() > 2 ? rest[2] : "";
        double at = util::parseDate(when);
        if (title.empty() || prompt.empty() || std::isnan(at) || (every && *every != "day" && *every != "week" && *every != "month")) {
            say("usage: claude-usage remind add <when, e.g. 2026-10-13T09:00> <title> <prompt> [--dir <path>] [--flags '<claude flags>'] [--every day|week|month]");
            return 1;
        }
        json::Value r = json::Value::object();
        std::string id = randomId();
        r.set("id", id);
        r.set("at", at);
        r.set("title", title);
        r.set("prompt", prompt);
        r.set("dir", dir.value_or(""));
        r.set("flags", flags.value_or(""));
        if (every && !every->empty()) {
            r.set("every", *every);
        }
        list.push(std::move(r));
        std::stable_sort(list.items().begin(), list.items().end(), [](const json::Value& a, const json::Value& b) { return a["at"].num() < b["at"].num(); });
        if (!reminders::saveRaw(list)) {
            warn("could not write " + util::utf8(config::remindersPath()));
            return 1;
        }
        say(id + "  " + util::localeString(at) + (every && !every->empty() ? " every " + *every : "") + "  " + title);
        return 0;
    }
    if (sub == "done") {
        std::string id = rest.empty() ? "" : rest[0];
        json::Value left = json::Value::array();
        for (const auto& r : list.items()) {
            if (r["id"].str() != id) {
                left.push(r);
            }
        }
        if (left.size() == list.size()) {
            say("no reminder " + (rest.empty() ? std::string("undefined") : id));
            return 1;
        }
        reminders::saveRaw(left);
        return 0;
    }
    if (sub == "list" || sub.empty()) {
        double now = util::nowMs();
        for (const auto& r : list.items()) {
            double at = r["at"].num(util::NaN);
            say(r["id"].str() + "  " + util::localeString(at) + (r["every"].truthy() ? " every " + r["every"].str() : "") + (at <= now ? " (due)" : "") + "  " +
                r["title"].str() + "  — " + r["dir"].str());
        }
        return 0;
    }
    say("usage: claude-usage remind add|list|done");
    return 1;
}

}  // namespace

void takeHomeFlags(std::vector<std::string>& args) {
    for (size_t i = 0; i < args.size();) {
        if (args[i] == "--home" && i + 1 < args.size()) {
            config::addHomeOverride(util::wide(args[i + 1]));
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        } else if (util::startsWith(args[i], "--home=")) {
            config::addHomeOverride(util::wide(args[i].substr(7)));
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
}

bool isCommand(const std::vector<std::string>& args) {
    if (args.empty()) {
        return false;
    }
    for (const char* c : {"tick", "status", "fit", "remind", "version", "--version", "help", "--help", "-h"}) {
        if (args[0] == c) {
            return true;
        }
    }
    return false;
}

int run(std::vector<std::string> args) {
    takeHomeFlags(args);
    std::string cmd = args.empty() ? "status" : args[0];
    try {
        if (cmd == "tick") return tick();
        if (cmd == "status") return status();
        if (cmd == "fit") return fitCommand();
        if (cmd == "remind") return remind(std::vector<std::string>(args.begin() + 1, args.end()));
        if (cmd == "version" || cmd == "--version") {
            say(std::string("claude-usage-win ") + VERSION_STRING);
            return 0;
        }
    } catch (const std::exception& e) {
        warn(e.what());
        return 1;
    }
    say("usage: claude-usage [--home <dir>]... tick|status|fit|remind|version");
    return cmd == "help" || cmd == "--help" || cmd == "-h" ? 0 : 1;
}

}  // namespace cli

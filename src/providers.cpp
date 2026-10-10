#include "providers.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "config.h"
#include "http.h"
#include "util.h"

namespace providers {

namespace {

const char* USAGE_URL = "https://api.anthropic.com/api/oauth/usage";
const char* PROFILE_URL = "https://api.anthropic.com/api/oauth/profile";
const char* CODEX_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage";
const char* CURSOR_USAGE_URL = "https://cursor.com/api/usage-summary";
const char* GEMINI_URL = "https://cloudcode-pa.googleapis.com/v1internal:";
const char* COPILOT_URL = "https://api.github.com/copilot_internal/user";

json::Value parseOrThrow(const std::string& body, const std::string& what) {
    json::Value j;
    if (!json::parse(body, j)) {
        throw std::runtime_error(what + ": the response is not JSON");
    }
    return j;
}

json::Value readJsonFile(const std::wstring& path) {
    std::string text;
    json::Value j;
    if (!util::readFile(path, text) || !json::parse(text, j)) {
        return json::Value();
    }
    return j;
}

json::Value jwtClaims(const std::string& token) {
    json::Value j;
    json::parse(util::jwtPayload(token), j);
    return j;
}

json::Value capOrNull(const json::Value& v) {
    return v.isStr() && !v.str().empty() ? json::Value(util::capitalize(v.str())) : json::Value();
}

json::Value strOrNull(const json::Value& v) { return v.truthy() && v.isStr() ? v : json::Value(); }

json::Value makeAcct(const std::string& id, const char* provider, json::Value email, json::Value plan, const std::vector<pace::Limit>& limits) {
    json::Value a = json::Value::object();
    a.set("id", id);
    a.set("provider", provider);
    a.set("email", std::move(email));
    a.set("plan", std::move(plan));
    a.set("limits", pace::toJson(limits));
    return a;
}

std::string kindFor(double windowMinutes) {
    return windowMinutes > pace::LONG_WINDOW_MIN ? "weekly_all" : "session";
}

}  // namespace

// ---------------------------------------------------------------- Claude
json::Value Profile::dirJson() const {
    if (isDefault()) {
        return homeIndex == 0 ? json::Value() : json::Value(util::utf8(util::join(home, L".claude")));
    }
    return json::Value(util::utf8(dir));
}

std::vector<Profile> claudeProfiles() {
    std::vector<Profile> out;
    const auto& homes = config::homes();
    for (size_t i = 0; i < homes.size(); i++) {
        const std::wstring& h = homes[i];
        // A home with no Claude Code in it at all (a plain %USERPROFILE% next to a container's home) is not a
        // failed login: it has nothing to read. Claude Code always writes .claude.json; a bare .claude folder
        // proves nothing, since this app's own state lives in .claude\claude-usage.
        std::wstring creds = util::join(util::join(h, L".claude"), L".credentials.json");
        if (util::exists(util::join(h, L".claude.json")) || util::exists(creds)) {
            Profile d;
            d.home = h;
            d.homeIndex = static_cast<int>(i);
            d.config = util::join(h, L".claude.json");
            d.credentials = creds;
            out.push_back(d);
        }
        std::vector<util::Entry> entries = util::listDir(h);
        std::sort(entries.begin(), entries.end(), [](const util::Entry& a, const util::Entry& b) { return a.name < b.name; });
        for (const auto& e : entries) {
            if (!e.dir || e.name.rfind(L".claude-", 0) != 0) {
                continue;
            }
            std::wstring dir = util::join(h, e.name);
            if (!util::exists(util::join(dir, L".claude.json"))) {
                continue;
            }
            Profile p;
            p.home = h;
            p.homeIndex = static_cast<int>(i);
            p.dir = dir;
            p.config = util::join(dir, L".claude.json");
            p.credentials = util::join(dir, L".credentials.json");
            out.push_back(p);
        }
    }
    return out;
}

std::optional<Credential> credential(const Profile& p) {
    std::string raw;
    if (!util::readFile(p.credentials, raw)) {
        if (p.isDefault()) {
            throw std::runtime_error("no Claude Code login in " + util::utf8(p.credentials));
        }
        return std::nullopt;   // a profile nobody has logged into yet is not an error
    }
    json::Value j;
    if (!json::parse(raw, j)) {
        throw std::runtime_error(util::utf8(p.credentials) + " is not JSON");
    }
    const json::Value& o = j["claudeAiOauth"];
    Credential c;
    c.accessToken = o["accessToken"].str();
    c.subscriptionType = o["subscriptionType"].str();
    c.rateLimitTier = o["rateLimitTier"].str();
    return c;
}

namespace {
http::Request claudeRequest(const Credential& c) {
    http::Request r;
    r.headers = {{"Authorization", "Bearer " + c.accessToken}, {"anthropic-beta", "oauth-2025-04-20"}};
    return r;
}
}  // namespace

std::vector<pace::Limit> fetchUsage(const Credential& c) {
    http::Response res = http::getWithRetry(USAGE_URL, claudeRequest(c), "usage");
    if (!res.ok()) {
        throw std::runtime_error("usage " + std::to_string(res.status));
    }
    json::Value j = parseOrThrow(res.body, "usage");
    std::vector<pace::Limit> out;
    // Two response shapes have been seen in the wild: `limits[]`, and top-level five_hour/seven_day objects.
    if (j["limits"].isArr()) {
        for (const auto& l : j["limits"].items()) {
            pace::Limit x;
            x.kind = l["kind"].str();
            x.scope = l["scope"]["model"]["display_name"].str();
            x.percent = l["percent"].isNum() ? l["percent"].num() : util::NaN;
            x.resetsAt = util::parseDate(l["resets_at"].str());
            out.push_back(x);
        }
        return out;
    }
    auto add = [&](const char* kind, const std::string& scope, const json::Value& v) {
        pace::Limit x;
        x.kind = kind;
        x.scope = scope;
        x.percent = v["utilization"].isNum() ? v["utilization"].num() : util::NaN;
        x.resetsAt = util::parseDate(v["resets_at"].str());
        out.push_back(x);
    };
    if (j["five_hour"].truthy()) {
        add("session", "", j["five_hour"]);
    }
    if (j["seven_day"].truthy()) {
        add("weekly_all", "", j["seven_day"]);
    }
    for (const auto& [k, v] : j.members()) {
        if (util::startsWith(k, "seven_day_") && v.isObj() && !v["utilization"].isNull()) {
            add("weekly_scoped", k.substr(10), v);
        }
    }
    return out;
}

Identity identity(const Credential& c, const Profile& p) {
    std::string tier;
    const std::string& t = c.rateLimitTier;
    for (size_t i = 0; i < t.size() && tier.empty(); i++) {
        if (!isdigit(static_cast<unsigned char>(t[i]))) {
            continue;
        }
        size_t e = i;
        while (e < t.size() && isdigit(static_cast<unsigned char>(t[e]))) {
            e++;
        }
        if (e < t.size() && t[e] == 'x') {
            tier = t.substr(i, e - i + 1);
        }
        i = e;
    }
    std::string sub = util::capitalize(c.subscriptionType);
    std::string planText = sub.empty() ? tier : tier.empty() ? sub : sub + " " + tier;
    json::Value plan = planText.empty() ? json::Value() : json::Value(planText);
    try {
        // Deliberately NOT getWithRetry: this is the display name, not the pacing figure, and the code below
        // falls back to the on-disk profile. It still needs the timeout: a fallback covers a failure, not a hang.
        http::Response res = http::fetch(PROFILE_URL, claudeRequest(c), std::max(1.0, http::attemptMs()));
        if (res.ok()) {
            json::Value j;
            if (json::parse(res.body, j) && j["account"]["uuid"].truthy()) {
                return {j["account"]["uuid"].str(), j["account"]["email"], plan};
            }
        }
    } catch (const std::exception&) {
    }
    json::Value o = readJsonFile(p.config)["oauthAccount"];
    if (o["accountUuid"].truthy()) {
        return {o["accountUuid"].str(), o["emailAddress"], plan};
    }
    return {"unknown", json::Value(), plan};
}

std::string modelFamily(const std::string& model) {
    std::string m = util::lower(model);
    for (const char* f : {"fable", "mythos", "opus", "sonnet", "haiku"}) {
        if (m.find(f) != std::string::npos) {
            return f;
        }
    }
    return m.empty() ? "" : "other";
}

pace::Ordered<pace::Tokens> scanTokens(double since, double until) {
    struct Seen {
        std::string model;
        pace::Tokens t;
    };
    std::unordered_map<std::string, size_t> index;
    std::vector<Seen> seen;
    std::vector<std::wstring> files;
    for (const auto& h : config::homes()) {
        util::jsonlFiles(util::join(util::join(h, L".claude"), L"projects"), files);
    }
    std::string text;
    for (const auto& f : files) {
        double mt = util::mtimeMs(f);
        if (!std::isfinite(mt) || mt < since) {
            continue;
        }
        if (!util::readFile(f, text)) {
            continue;
        }
        std::string fname = util::utf8(f);
        for (std::string_view line : util::splitLines(text)) {
            if (line.find("\"usage\"") == std::string_view::npos) {
                continue;
            }
            json::Value d;
            if (!json::parse(line, d)) {
                continue;
            }
            double t = util::parseDate(d["timestamp"].str());
            if (!(t > since && t <= until)) {
                continue;
            }
            const json::Value& u = d["message"]["usage"];
            if (!u.truthy()) {
                continue;
            }
            std::string key = d["requestId"].truthy() ? d["requestId"].str() : d["message"]["id"].truthy() ? d["message"]["id"].str() : fname + ":" + util::jsNum(t);
            Seen s;
            s.model = modelFamily(d["message"]["model"].str());
            s.t.input = u["input_tokens"].num(0);
            s.t.output = u["output_tokens"].num(0);
            s.t.cache_write = u["cache_creation_input_tokens"].num(0);
            s.t.cache_read = u["cache_read_input_tokens"].num(0);
            auto it = index.find(key);
            if (it != index.end()) {
                seen[it->second] = std::move(s);
            } else {
                index.emplace(std::move(key), seen.size());
                seen.push_back(std::move(s));
            }
        }
    }
    pace::Ordered<pace::Tokens> tokens;
    for (const auto& s : seen) {
        if (s.model.empty()) {
            continue;
        }
        pace::Tokens& row = tokens.at(s.model);
        row.input += s.t.input;
        row.output += s.t.output;
        row.cache_write += s.t.cache_write;
        row.cache_read += s.t.cache_read;
        row.requests++;
    }
    return tokens;
}

// ---------------------------------------------------------------- Codex
namespace {

std::vector<std::wstring> codexDirs() {
    std::vector<std::wstring> out;
    std::wstring env = util::envVar(L"CODEX_HOME");
    if (!env.empty()) {
        out.push_back(env);
    }
    for (const auto& h : config::homes()) {
        out.push_back(util::join(h, L".codex"));
    }
    return out;
}

struct Live {
    double t = 0;
    std::vector<pace::Limit> limits;
    json::Value email, plan;
};

Live codexLive(const json::Value& auth, double now) {
    http::Request r;
    r.headers = {{"Authorization", "Bearer " + auth["tokens"]["access_token"].str()},
                 {"ChatGPT-Account-Id", auth["tokens"]["account_id"].str()},
                 {"User-Agent", "claude-usage"},
                 {"Accept", "application/json"}};
    http::Response res = http::getWithRetry(CODEX_USAGE_URL, r, "codex usage");
    if (!res.ok()) {
        throw std::runtime_error("codex usage " + std::to_string(res.status));
    }
    json::Value j = parseOrThrow(res.body, "codex usage");
    Live live;
    live.t = now;
    for (const char* k : {"primary_window", "secondary_window"}) {
        const json::Value& w = j["rate_limit"][k];
        if (!w["used_percent"].isNum()) {
            continue;
        }
        pace::Limit l;
        l.kind = kindFor(w["limit_window_seconds"].isNum() ? w["limit_window_seconds"].num() / 60 : util::NaN);
        l.percent = w["used_percent"].num();
        l.resetsAt = w["reset_at"].truthy() ? w["reset_at"].num() * 1000 : util::NaN;
        live.limits.push_back(l);
    }
    if (live.limits.empty()) {
        throw std::runtime_error("codex usage: no windows");
    }
    live.email = j["email"];
    live.plan = j["plan_type"];
    return live;
}

std::optional<Reading> codexReading(const std::wstring& dir, double now, bool live) {
    json::Value auth = readJsonFile(util::join(dir, L"auth.json"));
    if (!auth.isObj()) {
        return std::nullopt;
    }
    json::Value claims = jwtClaims(auth["tokens"]["id_token"].str());
    const json::Value& info = claims["https://api.openai.com/auth"];
    std::string id = "codex:" + (auth["tokens"]["account_id"].truthy() ? auth["tokens"]["account_id"].str()
                                 : info["chatgpt_account_id"].truthy() ? info["chatgpt_account_id"].str()
                                                                         : std::string("unknown"));
    json::Value plan = capOrNull(info["chatgpt_plan_type"]);

    std::vector<pace::Sample> readings;
    std::vector<std::wstring> files;
    util::jsonlFiles(util::join(dir, L"sessions"), files);
    std::string text;
    for (const auto& f : files) {
        double mt = util::mtimeMs(f);
        if (!std::isfinite(mt) || now - mt > pace::FORGET_AFTER || !util::readFile(f, text)) {
            continue;
        }
        for (std::string_view line : util::splitLines(text)) {
            if (line.find("\"rate_limits\"") == std::string_view::npos) {
                continue;
            }
            json::Value d;
            if (!json::parse(line, d)) {
                continue;
            }
            const json::Value* rl = &d["payload"]["rate_limits"];
            if (!rl->truthy()) rl = &d["payload"]["info"]["rate_limits"];
            if (!rl->truthy()) rl = &d["rate_limits"];
            double t = util::parseDate(d["timestamp"].str());
            if (!(*rl)["primary"].truthy() || !std::isfinite(t)) {
                continue;
            }
            pace::Sample s;
            s.t = t;
            for (const char* k : {"primary", "secondary"}) {
                const json::Value& w = (*rl)[k];
                if (!w["used_percent"].isNum()) {
                    continue;
                }
                pace::Limit l;
                l.kind = kindFor(w["window_minutes"].isNum() ? w["window_minutes"].num() : util::NaN);
                l.percent = w["used_percent"].num();
                l.resetsAt = w["resets_at"].truthy()               ? w["resets_at"].num() * 1000
                             : w["resets_in_seconds"].isNum() ? t + w["resets_in_seconds"].num() * 1000
                                                               : util::NaN;
                s.limits.push_back(l);
            }
            readings.push_back(std::move(s));
        }
    }
    std::stable_sort(readings.begin(), readings.end(), [](const pace::Sample& a, const pace::Sample& b) { return a.t < b.t; });
    json::Value email = strOrNull(claims["email"]);
    if (live && auth["tokens"]["access_token"].truthy()) {
        try {
            Live r = codexLive(auth, now);
            pace::Sample s;
            s.t = r.t;
            s.limits = r.limits;
            readings.push_back(std::move(s));
            if (r.email.truthy()) {
                email = r.email;
            }
            if (r.plan.truthy() && r.plan.isStr()) {
                plan = util::capitalize(r.plan.str());
            }
        } catch (const std::exception& e) {
            util::err(std::string(e.what()) + "\n");
        }
    }
    if (readings.empty()) {
        return std::nullopt;
    }
    Reading out;
    out.acct = makeAcct(id, "codex", email, plan, readings.back().limits);
    out.asOf = readings.back().t;
    out.readings = std::move(readings);
    return out;
}

}  // namespace

std::vector<Reading> codexReadings(double now, bool live) {
    std::vector<Reading> out;
    std::set<std::wstring> done;
    for (const auto& dir : codexDirs()) {
        if (!done.insert(util::wide(util::lower(util::utf8(dir)))).second) {
            continue;
        }
        std::optional<Reading> r = codexReading(dir, now, live);
        if (!r) {
            continue;
        }
        // The same ChatGPT account reached from two homes is one account: keep the fresher reading.
        auto same = std::find_if(out.begin(), out.end(), [&](const Reading& x) { return x.acct["id"].str() == r->acct["id"].str(); });
        if (same == out.end()) {
            out.push_back(std::move(*r));
        } else if (r->asOf > same->asOf) {
            *same = std::move(*r);
        }
    }
    return out;
}

// ---------------------------------------------------------------- Cursor
namespace {

// Cursor keeps its login in a SQLite database. Windows 10+ ships SQLite as winsqlite3.dll, so it is read
// through that rather than a bundled copy or a sqlite3.exe on PATH.
class Sqlite {
public:
    Sqlite() {
        lib_ = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib_) {
            return;
        }
        open_ = reinterpret_cast<OpenFn>(GetProcAddress(lib_, "sqlite3_open_v2"));
        prepare_ = reinterpret_cast<PrepareFn>(GetProcAddress(lib_, "sqlite3_prepare_v2"));
        step_ = reinterpret_cast<StepFn>(GetProcAddress(lib_, "sqlite3_step"));
        text_ = reinterpret_cast<TextFn>(GetProcAddress(lib_, "sqlite3_column_text"));
        finalize_ = reinterpret_cast<StepFn>(GetProcAddress(lib_, "sqlite3_finalize"));
        close_ = reinterpret_cast<StepFn>(GetProcAddress(lib_, "sqlite3_close"));
        busy_ = reinterpret_cast<BusyFn>(GetProcAddress(lib_, "sqlite3_busy_timeout"));
    }
    ~Sqlite() {
        if (db_ && close_) {
            close_(db_);
        }
        if (lib_) {
            FreeLibrary(lib_);
        }
    }
    Sqlite(const Sqlite&) = delete;
    Sqlite& operator=(const Sqlite&) = delete;

    bool open(const std::wstring& path) {
        if (!open_ || !prepare_ || !step_ || !text_ || !finalize_ || !close_) {
            return false;
        }
        constexpr int SQLITE_OPEN_READONLY = 0x1;
        if (open_(util::utf8(path).c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != 0) {
            return false;
        }
        if (busy_) {
            busy_(db_, 2000);   // Cursor holds it open while it runs
        }
        return true;
    }

    std::string scalar(const std::string& sql) {
        void* stmt = nullptr;
        std::string out;
        if (prepare_(db_, sql.c_str(), -1, &stmt, nullptr) != 0 || !stmt) {
            return out;
        }
        constexpr int SQLITE_ROW = 100;
        if (step_(stmt) == SQLITE_ROW) {
            if (const unsigned char* t = text_(stmt, 0)) {
                out = reinterpret_cast<const char*>(t);
            }
        }
        finalize_(stmt);
        return util::trim(out);
    }

private:
    using OpenFn = int(WINAPI*)(const char*, void**, int, const char*);
    using PrepareFn = int(WINAPI*)(void*, const char*, int, void**, const char**);
    using StepFn = int(WINAPI*)(void*);
    using TextFn = const unsigned char*(WINAPI*)(void*, int);
    using BusyFn = int(WINAPI*)(void*, int);
    HMODULE lib_ = nullptr;
    void* db_ = nullptr;
    OpenFn open_ = nullptr;
    PrepareFn prepare_ = nullptr;
    StepFn step_ = nullptr, finalize_ = nullptr, close_ = nullptr;
    TextFn text_ = nullptr;
    BusyFn busy_ = nullptr;
};

}  // namespace

/// Cursor: the IDE's own login (state.vscdb) against the endpoint its dashboard reads. One window, the billing month.
std::vector<Reading> cursorReading(double now) {
    std::wstring db = util::join(util::envVar(L"APPDATA"), L"Cursor\\User\\globalStorage\\state.vscdb");
    if (!util::exists(db)) {
        return {};
    }
    Sqlite sql;
    if (!sql.open(db)) {
        return {};
    }
    auto get = [&](const char* k) { return sql.scalar(std::string("select value from ItemTable where key='cursorAuth/") + k + "'"); };
    std::string token = get("accessToken");
    if (token.empty()) {
        return {};
    }
    std::string sub = jwtClaims(token)["sub"].str();
    std::string uid = sub.substr(sub.find_last_of('|') == std::string::npos ? 0 : sub.find_last_of('|') + 1);
    http::Request r;
    r.headers = {{"Cookie", "WorkosCursorSessionToken=" + uid + "%3A%3A" + token}, {"User-Agent", "claude-usage"}, {"Accept", "application/json"}};
    http::Response res = http::getWithRetry(CURSOR_USAGE_URL, r, "cursor usage");
    if (!res.ok()) {
        throw std::runtime_error("cursor usage " + std::to_string(res.status));
    }
    json::Value j = parseOrThrow(res.body, "cursor usage");
    const json::Value& plan = j["individualUsage"]["plan"];
    // A free seat has no included usage: there is no limit to pace against, so it gets no tab.
    if (!plan["enabled"].truthy() || !(plan["limit"].num(0) > 0) || j["isUnlimited"].truthy()) {
        return {};
    }
    double start = util::parseDate(j["billingCycleStart"].str()), end = util::parseDate(j["billingCycleEnd"].str());
    pace::Limit l;
    l.kind = "monthly";
    l.percent = util::jsRound(plan["totalPercentUsed"].isNum() ? plan["totalPercentUsed"].num() : plan["used"].num(util::NaN) / plan["limit"].num() * 100);
    l.resetsAt = end;
    double wm = (end - start) / 60000;
    l.windowMin = std::isfinite(wm) && wm ? wm : util::NaN;
    std::string email = get("cachedEmail");
    Reading out;
    out.acct = makeAcct("cursor:" + uid, "cursor", email.empty() ? json::Value() : json::Value(email), capOrNull(j["membershipType"]), {l});
    out.asOf = now;
    return {out};
}

// ---------------------------------------------------------------- Gemini
/// Gemini CLI (Code Assist login). UNTESTED against a live account — written from the CLI's own `/stats` call.
/// Uses the CLI's stored access token as-is: it never refreshes one, so an expired token means "no reading"
/// and the account simply shows its last one until the CLI runs again. Quotas are per model, per day.
std::vector<Reading> geminiReading(double now) {
    std::vector<Reading> out;
    for (const auto& h : config::homes()) {
        json::Value creds = readJsonFile(util::join(h, L".gemini\\oauth_creds.json"));
        if (!creds.isObj() || !creds["access_token"].truthy() || (creds["expiry_date"].truthy() && creds["expiry_date"].num() < now + 60000)) {
            continue;
        }
        auto call = [&](const std::string& method, const json::Value& body) {
            http::Request r;
            r.method = "POST";
            r.headers = {{"Authorization", "Bearer " + creds["access_token"].str()}, {"Content-Type", "application/json"}, {"User-Agent", "claude-usage"}};
            r.body = json::dump(body);
            http::Response res = http::getWithRetry(GEMINI_URL + method, r, "gemini " + method);
            if (res.status == 403) {
                return json::Value::object();   // a personal account: Google moved those to Antigravity, no quota here
            }
            if (!res.ok()) {
                throw std::runtime_error("gemini " + method + " " + std::to_string(res.status));
            }
            return parseOrThrow(res.body, "gemini " + method);
        };
        json::Value meta = json::Value::object();
        meta.set("ideType", "IDE_UNSPECIFIED");
        meta.set("platform", "PLATFORM_UNSPECIFIED");
        meta.set("pluginType", "GEMINI");
        json::Value loadBody = json::Value::object();
        loadBody.set("metadata", meta);
        json::Value load = call("loadCodeAssist", loadBody);
        const json::Value& cp = load["cloudaicompanionProject"];
        std::string project = cp["id"].truthy() ? cp["id"].str() : cp.isStr() ? cp.str() : "";
        json::Value quotaBody = json::Value::object();
        if (!project.empty()) {
            quotaBody.set("project", project);
        }
        json::Value quota = call("retrieveUserQuota", quotaBody);
        std::vector<pace::Limit> limits;
        for (const auto& b : quota["buckets"].items()) {
            if (!b["remainingFraction"].isNum() || (b["tokenType"].truthy() && b["tokenType"].str() != "REQUESTS")) {
                continue;
            }
            pace::Limit l;
            l.kind = "daily";
            std::string model = b["modelId"].truthy() ? b["modelId"].str() : "all";
            l.scope = util::startsWith(model, "gemini-") ? model.substr(7) : model;
            l.percent = util::jsRound((1 - b["remainingFraction"].num()) * 100);
            l.resetsAt = b["resetTime"].truthy() ? util::parseDate(b["resetTime"].str()) : util::NaN;
            limits.push_back(l);
        }
        std::stable_sort(limits.begin(), limits.end(), [](const pace::Limit& a, const pace::Limit& b) { return a.percent > b.percent; });
        if (limits.size() > 3) {
            limits.resize(3);
        }
        if (limits.empty()) {
            continue;
        }
        json::Value email = strOrNull(jwtClaims(creds["id_token"].str())["email"]);
        std::string id = "gemini:" + (email.truthy() ? email.str() : !project.empty() ? project : std::string("default"));
        if (std::any_of(out.begin(), out.end(), [&](const Reading& x) { return x.acct["id"].str() == id; })) {
            continue;
        }
        Reading r;
        r.acct = makeAcct(id, "gemini", email, strOrNull(load["currentTier"]["name"]), limits);
        r.asOf = now;
        out.push_back(std::move(r));
    }
    return out;
}

// ---------------------------------------------------------------- Copilot
namespace {

/// gh's hosts.yml: the github.com entry's own oauth_token (the active account), else any token under it.
std::string tokenFromHostsYml(const std::wstring& path) {
    std::string text;
    if (!util::readFile(path, text)) {
        return {};
    }
    bool inHost = false;
    size_t childIndent = std::string::npos;
    std::string direct, nested;
    for (std::string_view line : util::splitLines(text)) {
        size_t indent = line.find_first_not_of(' ');
        if (indent == std::string_view::npos || line[indent] == '#') {
            continue;
        }
        std::string body = util::trim(line);
        if (indent == 0) {
            inHost = body == "github.com:";
            childIndent = std::string::npos;
            continue;
        }
        if (!inHost) {
            continue;
        }
        if (childIndent == std::string::npos) {
            childIndent = indent;
        }
        if (!util::startsWith(body, "oauth_token:")) {
            continue;
        }
        std::string v = util::trim(body.substr(12));
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) {
            v = v.substr(1, v.size() - 2);
        }
        if (indent == childIndent && direct.empty()) {
            direct = v;
        } else if (nested.empty()) {
            nested = v;
        }
    }
    return direct.empty() ? nested : direct;
}

}  // namespace

/// GitHub Copilot, through the gh login: `gh auth token` on Windows, then each home's gh config (a container's
/// gh keeps it in plain hosts.yml). UNTESTED against a paid seat. Premium requests are the binding quota.
std::vector<Reading> copilotReading(double now) {
    std::vector<std::string> tokens;
    auto add = [&](std::string t) {
        t = util::trim(t);
        if (!t.empty() && std::find(tokens.begin(), tokens.end(), t) == tokens.end()) {
            tokens.push_back(t);
        }
    };
    util::RunResult gh = util::run(L"gh auth token", 5000);
    if (gh.started && !gh.timedOut && gh.exitCode == 0) {
        add(gh.out);
    }
    add(tokenFromHostsYml(util::join(util::envVar(L"APPDATA"), L"GitHub CLI\\hosts.yml")));
    for (const auto& h : config::homes()) {
        add(tokenFromHostsYml(util::join(h, L".config\\gh\\hosts.yml")));
    }
    std::vector<Reading> out;
    for (const auto& token : tokens) {
        http::Request r;
        r.headers = {{"Authorization", "token " + token}, {"Editor-Version", "vscode/1.95.0"}, {"User-Agent", "claude-usage"}};
        http::Response res = http::getWithRetry(COPILOT_URL, r, "copilot");
        if (!res.ok()) {
            throw std::runtime_error("copilot " + std::to_string(res.status));
        }
        json::Value j = parseOrThrow(res.body, "copilot");
        const json::Value& q = j["quota_snapshots"]["premium_interactions"];
        if (!q.isObj() || q["unlimited"].truthy() || !q["percent_remaining"].isNum()) {
            continue;
        }
        pace::Limit l;
        l.kind = "monthly";
        l.percent = util::jsRound(100 - q["percent_remaining"].num());
        l.resetsAt = j["quota_reset_date"].truthy() ? util::parseDate(j["quota_reset_date"].str()) : util::NaN;
        std::string id = "copilot:" + j["login"].str();
        if (std::any_of(out.begin(), out.end(), [&](const Reading& x) { return x.acct["id"].str() == id; })) {
            continue;
        }
        Reading rd;
        rd.acct = makeAcct(id, "copilot", j["login"], capOrNull(j["copilot_plan"]), {l});
        rd.asOf = now;
        out.push_back(std::move(rd));
    }
    return out;
}

// ---------------------------------------------------------------- Antigravity
namespace {

std::wstring commandLineOf(DWORD pid) {
    using QueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static QueryFn query = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query) {
        return {};
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return {};
    }
    constexpr ULONG ProcessCommandLineInformation = 60;   // Windows 8.1+
    ULONG len = 0;
    query(h, ProcessCommandLineInformation, nullptr, 0, &len);
    std::wstring out;
    if (len) {
        std::vector<BYTE> buf(len);
        if (query(h, ProcessCommandLineInformation, buf.data(), len, &len) >= 0) {
            auto* us = reinterpret_cast<UNICODE_STRING*>(buf.data());
            out.assign(us->Buffer, us->Length / sizeof(wchar_t));
        }
    }
    CloseHandle(h);
    return out;
}

std::vector<int> listeningPorts(DWORD pid) {
    std::vector<int> ports;
    auto add = [&](DWORD raw) {
        int port = static_cast<int>(((raw & 0xFF) << 8) | ((raw >> 8) & 0xFF));   // network byte order
        if (std::find(ports.begin(), ports.end(), port) == ports.end()) {
            ports.push_back(port);
        }
    };
    for (ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
        if (!size) {
            continue;
        }
        std::vector<BYTE> buf(size);
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) {
            continue;
        }
        if (family == AF_INET) {
            auto* t = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; i++) {
                if (t->table[i].dwOwningPid == pid) {
                    add(t->table[i].dwLocalPort);
                }
            }
        } else {
            auto* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; i++) {
                if (t->table[i].dwOwningPid == pid) {
                    add(t->table[i].dwLocalPort);
                }
            }
        }
    }
    return ports;
}

std::string family(const std::string& label) {
    std::string l = util::lower(label);
    auto seq = [&](const char* a, const char* b) {
        size_t p = l.find(a);
        return p != std::string::npos && l.find(b, p + strlen(a)) != std::string::npos;
    };
    if (seq("gemini", "pro")) return "Gemini Pro";
    if (l.find("gemini") != std::string::npos) return "Gemini Flash";
    if (seq("claude", "opus")) return "Claude Opus";
    if (l.find("claude") != std::string::npos) return "Claude Sonnet";
    // `label.replace(/\s*\(.*\)$/, '')`: drop a trailing parenthetical, from the first "(".
    if (!label.empty() && label.back() == ')') {
        size_t open = label.find('(');
        if (open != std::string::npos) {
            size_t cut = open;
            while (cut > 0 && isspace(static_cast<unsigned char>(label[cut - 1]))) {
                cut--;
            }
            return label.substr(0, cut);
        }
    }
    return label;
}

std::string vendorOf(const std::string& label) {
    std::string l = util::lower(label);
    if (l.find("gemini") != std::string::npos) return "gemini";
    if (l.find("claude") != std::string::npos) return "claude";
    if (l.find("gpt") != std::string::npos || l.find("openai") != std::string::npos) return "openai";
    return "other";
}

}  // namespace

/// Antigravity (Google's agent app — where personal Gemini accounts now live). The quota is only held by the
/// app's own local language server, so this reads it while the app is running and otherwise returns nothing,
/// which leaves the account on its last reading. Every model has its own allowance; models are folded into
/// families (the worst of each), since fourteen rows of "Flash (Low)" say nothing a family row doesn't.
std::vector<Reading> antigravityReading(double now) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return {};
    }
    DWORD pid = 0;
    std::string csrf;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !pid; ok = Process32NextW(snap, &pe)) {
        if (!util::icontains(util::utf8(pe.szExeFile), "language_server")) {
            continue;
        }
        std::string cmd = util::utf8(commandLineOf(pe.th32ProcessID));
        if (!util::icontains(cmd, "antigravity") || cmd.find("--csrf_token") == std::string::npos) {
            continue;
        }
        size_t at = cmd.find("--csrf_token") + 12;
        if (at < cmd.size() && (cmd[at] == '=' || cmd[at] == ' ')) {
            at++;
        }
        size_t end = cmd.find_first_of(" \t", at);
        csrf = cmd.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (csrf.size() >= 2 && csrf.front() == '"' && csrf.back() == '"') {
            csrf = csrf.substr(1, csrf.size() - 2);
        }
        pid = pe.th32ProcessID;
    }
    CloseHandle(snap);
    if (!pid) {
        return {};
    }
    json::Value status;
    for (int port : listeningPorts(pid)) {   // it listens on an https port and a plain one; only the plain one answers
        http::Request r;
        r.method = "POST";
        r.headers = {{"Content-Type", "application/json"}, {"Connect-Protocol-Version", "1"}, {"X-Codeium-Csrf-Token", csrf}};
        r.body = R"({"metadata":{"ideName":"antigravity","extensionName":"antigravity","locale":"en"}})";
        try {
            // Deliberately NOT getWithRetry: a LOCAL language server, where "not running" is the ordinary answer
            // rather than a blip. Retrying a closed port would delay the tick to learn nothing.
            http::Response res = http::fetch("http://127.0.0.1:" + std::to_string(port) + "/exa.language_server_pb.LanguageServerService/GetUserStatus", r, http::LOCAL_PROBE_MS);
            json::Value j;
            if (res.ok() && json::parse(res.body, j) && j["userStatus"].isObj()) {
                status = j["userStatus"];
                break;
            }
        } catch (const std::exception&) {
        }
    }
    if (!status.isObj()) {
        return {};
    }
    // Antigravity is a wrapper: one Google plan reselling several vendors' models, each with its own allowance.
    // So it is not one tab — it is one tab per VENDOR (whose model you are spending), marked "via Antigravity".
    pace::Ordered<pace::Ordered<pace::Limit>> byVendor;
    for (const auto& m : status["cascadeModelConfigData"]["clientModelConfigs"].items()) {
        const json::Value& q = m["quotaInfo"];
        if (!q["remainingFraction"].isNum() && !q["resetTime"].truthy()) {
            continue;
        }
        std::string lbl = m["label"].str();
        double percent = util::jsRound((1 - q["remainingFraction"].num(0)) * 100);
        std::string f = family(lbl.empty() ? "model" : lbl);
        auto& rows = byVendor.at(vendorOf(lbl));
        double resetsAt = q["resetTime"].truthy() ? util::parseDate(q["resetTime"].str()) : util::NaN;
        pace::Limit* existing = rows.find(f);
        if (existing && !(percent > existing->percent)) {
            continue;
        }
        pace::Limit l;
        l.kind = "quota";
        l.scope = f;
        l.percent = percent;
        l.resetsAt = resetsAt;
        // The app reports one reset per model and no window length; a reset days away is a weekly allowance, a near one a short window.
        l.windowMin = std::isfinite(resetsAt) && resetsAt && resetsAt - now < pace::DAY_MS ? 5 * 60 : 7 * 24 * 60;
        rows.at(f) = l;
    }
    std::string email = status["email"].str();
    json::Value plan = strOrNull(status["planStatus"]["planInfo"]["planName"]);
    std::vector<Reading> out;
    for (auto& [vendor, rows] : byVendor.items) {
        std::vector<pace::Limit> limits;
        for (auto& [k, l] : rows.items) {
            limits.push_back(l);
        }
        std::stable_sort(limits.begin(), limits.end(), [](const pace::Limit& a, const pace::Limit& b) {
            if (a.percent != b.percent) return a.percent > b.percent;
            return a.scope < b.scope;
        });
        if (limits.size() > 3) {
            limits.resize(3);
        }
        const char* title = vendor == "gemini" ? "Gemini" : vendor == "claude" ? "Claude" : vendor == "openai" ? "GPT-OSS" : "Other models";
        json::Value a = json::Value::object();
        a.set("id", "antigravity:" + (email.empty() ? std::string("default") : email) + ":" + vendor);
        a.set("provider", "antigravity");
        a.set("vendor", vendor);
        a.set("title", title);
        a.set("via", "Antigravity");
        a.set("email", email.empty() ? json::Value() : json::Value(email));
        a.set("plan", plan);
        a.set("limits", pace::toJson(limits));
        Reading r;
        r.acct = std::move(a);
        r.asOf = now;
        out.push_back(std::move(r));
    }
    return out;
}

const std::vector<Provider>& others() {
    static const std::vector<Provider> list = {cursorReading, geminiReading, antigravityReading, copilotReading};
    return list;
}

}  // namespace providers

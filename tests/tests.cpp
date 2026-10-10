// usage_tests <path to claude-usage.exe> <path to src>
// `claude-usage status` and `tick` on fixtures in a scratch home, the retry policy against a real local server
// (a stub would only prove the stub), the bare-fetch scan, the reset-cycle id, and unit tests for JSON, dates,
// the pace model and reminders.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "http.h"
#include "json.h"
#include "pace.h"
#include "reminders.h"
#include "ui_model.h"
#include "util.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
        failures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

template <class A, class B>
void eq(const A& a, const B& b, const std::string& what) {
    bool ok = a == b;
    if (!ok) {
        if constexpr (std::is_convertible_v<A, std::string> && std::is_convertible_v<B, std::string>) {
            check(false, what + " — got [" + std::string(a) + "] want [" + std::string(b) + "]");
            return;
        } else if constexpr (std::is_arithmetic_v<A> && std::is_arithmetic_v<B>) {
            check(false, what + " — got " + json::number(static_cast<double>(a)) + " want " + json::number(static_cast<double>(b)));
            return;
        }
    }
    check(ok, what);
}

std::string cliExe, srcDir;

std::wstring scratchHome() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    static int n = 0;
    std::wstring dir = std::wstring(tmp) + L"claude-usage-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n);
    util::makeDirs(util::join(util::join(dir, L".claude"), L"claude-usage"));
    return dir;
}

util::RunResult runCli(const std::wstring& home, const std::wstring& args) {
    SetEnvironmentVariableW(L"CLAUDE_USAGE_HOMES", home.c_str());
    util::RunResult r = util::run(util::quoteArg(util::wide(cliExe)) + L" " + args, 60000);
    SetEnvironmentVariableW(L"CLAUDE_USAGE_HOMES", nullptr);
    return r;
}

json::Value readJson(const std::wstring& path) {
    std::string text;
    json::Value j;
    util::readFile(path, text);
    json::parse(text, j);
    return j;
}

// ---------------------------------------------------------------- a tiny HTTP server
class Server {
public:
    /// handler(hit) → status to answer with, or 0 to accept and never answer.
    explicit Server(std::function<int(int)> handler) : handler_(std::move(handler)) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        listen(listen_, 16);
        int len = sizeof a;
        getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~Server() {
        stop_ = true;
        closesocket(listen_);
        thread_.join();
        for (SOCKET s : held_) {
            closesocket(s);
        }
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }
    int hits() const { return hits_; }

private:
    void serve() {
        while (!stop_) {
            SOCKET c = accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) {
                return;
            }
            std::string req;
            char buf[2048];
            while (req.find("\r\n\r\n") == std::string::npos) {
                int n = recv(c, buf, sizeof buf, 0);
                if (n <= 0) {
                    break;
                }
                req.append(buf, static_cast<size_t>(n));
            }
            int status = handler_(++hits_);
            if (!status) {
                held_.push_back(c);   // accept, never answer
                continue;
            }
            std::string body = status == 200 ? R"({"ok":true})" : "";
            std::string resp = "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                               "\r\nConnection: close\r\n\r\n" + body;
            send(c, resp.data(), static_cast<int>(resp.size()), 0);
            shutdown(c, SD_SEND);
            closesocket(c);
        }
    }

    std::function<int(int)> handler_;
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<int> hits_{0};
    std::atomic<bool> stop_{false};
    std::vector<SOCKET> held_;
    std::thread thread_;
};

double since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------- tests
void testJson() {
    json::Value v;
    check(json::parse(R"({"a":[1,2.5,"x\u00e9\ud83d\ude00",true,null],"b":{"c":-0.125e2}})", v), "parse a nested document");
    eq(v["a"][2].str(), std::string("x\xC3\xA9\xF0\x9F\x98\x80"), "\\u escapes and surrogate pairs decode to UTF-8");
    eq(v["b"]["c"].num(), -12.5, "exponent");
    eq(json::dump(v), std::string(R"({"a":[1,2.5,"xé😀",true,null],"b":{"c":-12.5}})"), "compact dump");
    eq(json::dump(json::Value::array(), 2), std::string("[]"), "empty array");
    json::Value o = json::Value::object();
    o.set("k", 1);
    o.set("arr", json::Value::array()).push(2);
    eq(json::dump(o, 2), std::string("{\n  \"k\": 1,\n  \"arr\": [\n    2\n  ]\n}"), "JSON.stringify(v, null, 2) layout");
    o.set("k", 3);
    eq(json::dump(o), std::string(R"({"k":3,"arr":[2]})"), "set keeps the key's position");
    check(!json::parse("{\"a\":1,}", v), "a trailing comma is rejected");
    check(!json::parse("[1] x", v), "trailing garbage is rejected");
    eq(json::number(1760000000000.0), std::string("1760000000000"), "epoch ms print as integers");
    eq(json::number(0.1), std::string("0.1"), "shortest digits");
    eq(json::number(1e21), std::string("1e+21"), "exponential from 1e21");
    eq(json::number(1.5e-7), std::string("1.5e-7"), "exponential below 1e-6");
    eq(json::number(0.000001), std::string("0.000001"), "fixed down to 1e-6");
    eq(json::number(-0.0), std::string("0"), "negative zero prints as 0");
    eq(json::number(123.456), std::string("123.456"), "fraction");
    json::Value nan = json::Value(util::NaN);
    eq(json::dump(nan), std::string("null"), "NaN writes null");
}

void testDates() {
    eq(util::parseDate("2026-10-10T17:00:00Z"), 1791651600000.0, "UTC");
    eq(util::parseDate("2026-10-10T17:00:00.123456+00:00"), 1791651600123.0, "microseconds truncate to ms");
    eq(util::parseDate("2026-10-10T19:00:00+02:00"), 1791651600000.0, "offset");
    eq(util::parseDate("2026-10-10"), 1791590400000.0, "a bare date is UTC midnight");
    check(std::isnan(util::parseDate("")), "empty is NaN");
    check(std::isnan(util::parseDate("tomorrow")), "garbage is NaN");
    check(std::isnan(util::parseDate("2026-13-01")), "month 13 is NaN");
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 9;
    tm.tm_mday = 13;
    tm.tm_hour = 9;
    tm.tm_isdst = -1;
    eq(util::parseDate("2026-10-13T09:00"), static_cast<double>(mktime(&tm)) * 1000, "a date-time without an offset is local");
    eq(util::isoString(1791651600123.0), std::string("2026-10-10T17:00:00.123Z"), "toISOString");
}

void testPace() {
    double now = 1791651600000.0;
    // A week window half elapsed at 20% lands at 40.
    pace::Limit week;
    week.kind = "weekly_all";
    week.percent = 20;
    week.resetsAt = now + 3.5 * pace::DAY_MS;
    auto ws = pace::windows({week}, {}, now);
    check(ws.size() == 1 && ws[0].isLong, "a week is a long window");
    check(std::abs(ws[0].projected - 40) < 1e-9, "projected = used / elapsed (" + json::number(ws[0].projected) + ")");
    eq(pace::verdict(ws).level, std::string("green"), "40 is green");

    // The session: rate over the last 45 minutes, projected to the reset.
    pace::Limit sess;
    sess.kind = "session";
    sess.percent = 50;
    sess.resetsAt = now + 120 * 60000;
    pace::Sample old;
    old.t = now - 30 * 60000;
    pace::Limit sOld = sess;
    sOld.percent = 35;
    old.limits = {sOld};
    pace::Samples hist = {&old};
    auto sw = pace::windows({sess}, hist, now);
    check(std::abs(sw[0].projected - (50 + 0.5 * 120)) < 1e-9, "session projects its 45-minute rate (" + json::number(sw[0].projected) + ")");
    eq(pace::verdict(sw).level, std::string("red"), "110 is red");
    auto advice = pace::advise(sw, hist, nullptr, now);
    check(advice && util::startsWith(*advice, "this session is burning fast — ease off for the next 2h (about 17% less)"), "session advice: " + advice.value_or("(none)"));

    // Used up.
    pace::Limit full = week;
    full.percent = 100;
    auto fw = pace::windows({full}, {}, now);
    eq(pace::advise(fw, {}, nullptr, now).value_or(""), std::string("the week is used up — wait 84h for the reset"), "used-up advice");
    eq(pace::verdict(fw).level, std::string("red"), "a used-up window is red");

    // A passed reset rolls the window to idle at 0%.
    pace::Limit passed = week;
    passed.percent = 95;
    passed.resetsAt = now - 60000;
    auto rw = pace::windows(pace::rolled({passed}, now), {}, now);
    check(rw[0].idle && rw[0].percent == 0, "rolled → idle at 0%");

    // Standby wording.
    pace::Limit nearly = week;
    nearly.percent = 92;
    nearly.resetsAt = now + (26 * 60 + 5) * 60000.0;
    eq(pace::standby(pace::windows({nearly}, {}, now)), std::string("week nearly used — back in 1d 2h"), "standby: nearly used");
    eq(pace::standby(rw), std::string("ready to switch back — everything has reset"), "standby: reset");

    pace::Window daily;
    daily.kind = "daily";
    eq(pace::label(daily), std::string("today's quota"), "label collapses the double space");

    // NNLS recovers non-negative weights.
    std::vector<std::vector<double>> X = {{1, 0}, {0, 1}, {1, 1}, {2, 1}};
    std::vector<double> y = {2, 3, 5, 7};
    auto w = pace::nnls(X, y);
    check(w && std::abs((*w)[0] - 2) < 1e-3 && std::abs((*w)[1] - 3) < 1e-3, "nnls recovers 2, 3");

    // Cost: list prices, cache write 1.25×, cache read 0.1× input.
    pace::Ordered<pace::Tokens> t;
    t.at("opus") = {1e6, 1e6, 1e6, 1e6, 1};
    eq(pace::costOf(t).total, 5 + 25 + 6.25 + 0.5, "API-equivalent cost");
}

void testCycleId() {
    // Walk one whole weekly cycle at the real 10-minute tick: the id must not drift, and a turnover must jump.
    double readEvery = 600, window = 7 * 24 * 3600.0, t = 1700000000000.0, left = window;
    long long first = ui::cycleId(t, left / 3600), worst = 0;
    while (left > readEvery) {
        t += readEvery * 1000;
        left -= readEvery;
        worst = std::max(worst, std::llabs(ui::cycleId(t, left / 3600) - first));
    }
    check(worst <= 600, "the cycle id is stable within a cycle (drift " + std::to_string(worst) + ")");
    eq(worst, 0LL, "zero drift across a full cycle");
    long long after = ui::cycleId(t + readEvery * 1000, window / 3600);
    check(after - first >= static_cast<long long>(window) - 600, "a real turnover moves the id by the whole window");
}

void testReminders() {
    reminders::Reminder r;
    r.dir = "C:\\Users\\me\\it's";
    r.prompt = "say 'hi'";
    r.flags = "--chrome";
    eq(reminders::command(r), std::string("Set-Location -LiteralPath 'C:\\Users\\me\\it''s'; claude --chrome 'say ''hi'''"), "PowerShell command");
    r.dir = "/home/me/code";
    r.flags = "";
    eq(reminders::command(r), std::string("cd '/home/me/code' && claude 'say '\\''hi'\\'''"), "POSIX command for a container path");

    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 31;
    tm.tm_hour = 9;
    tm.tm_isdst = -1;
    double jan31 = static_cast<double>(mktime(&tm)) * 1000;
    auto next = reminders::nextDate(jan31, "month", jan31);
    time_t nt = static_cast<time_t>(*next / 1000);
    std::tm out{};
    localtime_s(&out, &nt);
    check(out.tm_mon == 1 && out.tm_mday == 28 && out.tm_hour == 9, "Jan 31 + a month = Feb 28, same time");
    auto skip = reminders::nextDate(jan31, "day", jan31 + 3.5 * pace::DAY_MS);
    check(skip && *skip > jan31 + 3.5 * pace::DAY_MS && *skip <= jan31 + 4.6 * pace::DAY_MS, "missed days are skipped, not replayed");
    check(!reminders::nextDate(jan31, "year", jan31), "unknown unit");
}

/// A weekly window whose reset has passed but which the server has not turned over yet: it reads as idle at
/// 0%, and printing it must not crash (an idle window has no allowedPerHour to format).
void testStatusIdle() {
    std::wstring home = scratchHome();
    double now = util::nowMs();
    json::Value sample = json::Value::object();
    sample.set("t", now);
    sample.set("acct", "a");
    json::Value limits = json::Value::array();
    json::Value s = json::Value::object();
    s.set("kind", "session");
    s.set("scope", json::Value());
    s.set("percent", 40);
    s.set("resetsAt", now + 3600000);
    limits.push(s);
    json::Value w = json::Value::object();
    w.set("kind", "weekly_all");
    w.set("scope", json::Value());
    w.set("percent", 95);
    w.set("resetsAt", now - 60000);
    limits.push(w);
    sample.set("limits", limits);
    sample.set("tokens", json::Value::object());
    util::appendFile(util::join(home, L".claude\\claude-usage\\samples.jsonl"), json::dump(sample) + "\n");

    util::RunResult run = runCli(home, L"status");
    check(run.started && run.exitCode == 0, "claude-usage status exited " + std::to_string(run.exitCode) + ":\n" + run.out);
    json::Value st = readJson(util::join(home, L".claude\\claude-usage\\status.json"));
    const json::Value* week = nullptr;
    for (const auto& x : st["windows"].items()) {
        if (x["key"].str() == "weekly_all") {
            week = &x;
        }
    }
    check(week && (*week)["percent"].num(-1) == 0, "the passed week reads 0%");
    check(week && (*week)["idle"].boolean(), "the passed week is idle");
    check(run.out.find("weekly_all") != std::string::npos && run.out.find("  0% \xC2\xB7 idle") != std::string::npos, "status prints the idle week:\n" + run.out);
    check(st["pace"].num(999) < 95, "a passed reset must not set the pace");
}

/// No Claude login anywhere: the tick still writes status.json, with the error and its time, and fails.
void testTickWithoutLogin() {
    std::wstring home = scratchHome();
    util::RunResult run = runCli(home, L"tick");
    eq(static_cast<int>(run.exitCode), 1, "a tick with no login exits 1");
    json::Value st = readJson(util::join(home, L".claude\\claude-usage\\status.json"));
    check(util::startsWith(st["error"].str(), "usage unreadable: no Claude Code login found"), "status.error says why: " + st["error"].str());
    check(st["errorAt"].isNum() && st["accountsAt"].isNum() && st["accounts"].isArr(), "errorAt / accountsAt / accounts are written");

    // Claude Code is there (.claude.json) but logged out: the error names the missing credential.
    std::wstring out = scratchHome();
    util::appendFile(util::join(out, L".claude.json"), "{}");
    run = runCli(out, L"tick");
    eq(static_cast<int>(run.exitCode), 1, "a logged-out tick exits 1");
    st = readJson(util::join(out, L".claude\\claude-usage\\status.json"));
    check(st["error"].str().find("no Claude Code login in ") != std::string::npos && st["error"].str().find(".credentials.json") != std::string::npos,
          "a logged-out home says which credential is missing: " + st["error"].str());
}

void testRetryBudgetConstants() {
    double sum = 0;
    for (double d : http::RETRY_MS) {
        sum += d;
    }
    // Pinned before the waits are shortened below: every attempt's timeout is clamped to what is left of the
    // budget, so it caps the WHOLE read phase, and it has to stay inside the 45 s the card waits after a click.
    check(sum <= http::READ_BUDGET_MS, "the backoffs must fit one run budget");
    check(http::ATTEMPT_MS <= http::READ_BUDGET_MS, "one attempt must not exceed the whole read budget");
    // The budget covers only reads that go through getWithRetry. The Antigravity probe runs AFTER it and spends
    // up to LOCAL_PROBE_MS per listening port, so the headroom under the wait has to be stated with it in.
    check(http::READ_BUDGET_MS + 2 * http::LOCAL_PROBE_MS <= http::CLICK_POLL_MS, "the read budget plus the antigravity probe must fit the click-to-poll wait");
}

void testRetryPolicy() {
    http::RETRY_MS = {5, 5};
    http::clearReadBudget();
    {
        // A blip is retried and the good answer is the one returned.
        Server s([](int hit) { return hit <= 2 ? 429 : 200; });
        http::Response res = http::getWithRetry(s.url(), {}, "test");
        eq(s.hits(), 3, "a 429 must be retried until it answers");
        eq(res.body, std::string(R"({"ok":true})"), "the good answer is returned");
    }
    {
        // An auth failure is NOT retried: tokens are never refreshed, so the next 401 is the same 401.
        Server s([](int) { return 401; });
        int status = 0;
        std::string threw;
        try {
            status = http::getWithRetry(s.url(), {}, "test").status;
        } catch (const std::exception& e) {
            threw = e.what();
        }
        eq(s.hits(), 1, "a 401 must not be retried");
        check(threw.empty(), "a non-retryable status is returned, not thrown: " + threw);
        eq(status, 401, "the caller owns its own non-OK handling");
    }
    {
        // A sustained outage gives up rather than hanging the tick, and says which read failed.
        Server s([](int) { return 503; });
        std::string threw;
        try {
            http::getWithRetry(s.url(), {}, "cursor usage");
        } catch (const std::exception& e) {
            threw = e.what();
        }
        eq(threw, std::string("cursor usage 503"), "the error names the read");
        eq(s.hits(), 3, "it must stop after the budget, not retry for ever");
    }
    {
        // A spent budget stops the reads dead instead of letting each one start a fresh timeout.
        Server s([](int) { return 0; });
        http::startReadBudget(util::nowMs() - http::READ_BUDGET_MS - 1000);
        auto t0 = std::chrono::steady_clock::now();
        std::string threw;
        try {
            http::getWithRetry(s.url(), {}, "late");
        } catch (const std::exception& e) {
            threw = e.what();
        }
        check(threw.find("read budget spent") != std::string::npos, "a spent budget says so: " + threw);
        check(since(t0) < 500, "a spent budget must fail fast, not open another 15s timeout");
        eq(s.hits(), 0, "and it must not even reach the server");

        // ...and an attempt cannot OUTLIVE the budget either: this is the half that actually caps the tick.
        http::startReadBudget(util::nowMs() - http::READ_BUDGET_MS + 1500);
        auto t1 = std::chrono::steady_clock::now();
        threw.clear();
        try {
            http::getWithRetry(s.url(), {}, "tail");
        } catch (const std::exception& e) {
            threw = e.what();
        }
        check(!threw.empty(), "a hung read throws");
        check(since(t1) < 3000, "an attempt must be clamped to the rest of the budget (took " + json::number(std::round(since(t1))) + "ms)");
        http::clearReadBudget();
    }
}

/// The recurrence risk is not the call sites there are, it is the next provider someone adds with a bare
/// fetch. Every remote read goes through getWithRetry; the two exceptions are local or have a file fallback,
/// and each says so on the call's line.
void testNoBareFetch() {
    std::vector<std::string> bad;
    for (const auto& e : util::listDir(util::wide(srcDir))) {
        std::string name = util::utf8(e.name);
        if (e.dir || name == "http.cpp" || name.size() < 4 || name.substr(name.size() - 4) != ".cpp") {
            continue;
        }
        std::string text;
        util::readFile(util::join(util::wide(srcDir), e.name), text);
        int n = 0;
        for (std::string_view line : util::splitLines(text)) {
            n++;
            if (line.find("http::fetch(") == std::string_view::npos) {
                continue;
            }
            if (line.find("127.0.0.1") != std::string_view::npos || line.find("PROFILE_URL") != std::string_view::npos) {
                continue;
            }
            bad.push_back(name + ":" + std::to_string(n));
        }
    }
    std::string list;
    for (const auto& b : bad) {
        list += " " + b;
    }
    check(bad.empty(), "remote read(s) bypassing getWithRetry:" + list);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: usage_tests <claude-usage.exe> <src dir>\n");
        return 2;
    }
    cliExe = argv[1];
    srcDir = argv[2];
    testJson();
    testDates();
    testPace();
    testCycleId();
    testReminders();
    testStatusIdle();
    testTickWithoutLogin();
    testRetryBudgetConstants();
    testNoBareFetch();
    testRetryPolicy();
    std::printf("%d checks, %d failed\n", checks, failures);
    if (!failures) {
        std::printf("ok\n");
    }
    return failures ? 1 : 0;
}

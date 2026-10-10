#pragma once
// The pace model: windows → speed and projection, the per-model weight fit,
// the one-sentence advice, API-equivalent cost, and the remembered accounts. Pure functions over samples —
// no I/O — so the tests can drive them directly.
//
// The PACE number: for each limit window, where you will land at reset if you keep the average speed shown
// since the window opened (long windows) or the last 45 min (the 5-hour session). The headline is the worst
// window. 100 = you run out exactly at reset. >80 amber, >100 red.
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "json.h"
#include "util.h"

namespace pace {

constexpr double TARGET = 80;
constexpr double LONG_WINDOW_MIN = 6 * 60;
constexpr double RATE_WINDOW_MIN = 45;
constexpr double MIN_ELAPSED = 0.02;
constexpr double DAY_MS = 24 * 3600000.0;
constexpr double FORGET_AFTER = 7 * DAY_MS;   // an account not seen for a week drops off
/// Past this a reading is not live: grey rings, "last checked". `status` and the tray share it.
constexpr double STALE_AFTER_MS = 30 * 60000.0;

/// A string-keyed map that keeps insertion order, as a JS object does — the order shows in the files.
template <class T>
struct Ordered {
    std::vector<std::pair<std::string, T>> items;

    T* find(const std::string& k) {
        for (auto& [key, v] : items) {
            if (key == k) {
                return &v;
            }
        }
        return nullptr;
    }
    const T* find(const std::string& k) const { return const_cast<Ordered*>(this)->find(k); }
    T& at(const std::string& k) {
        if (T* v = find(k)) {
            return *v;
        }
        items.emplace_back(k, T{});
        return items.back().second;
    }
    bool empty() const { return items.empty(); }
};

/// One limit window as a provider reports it. NaN = null/absent; an empty scope = null.
struct Limit {
    std::string kind;
    std::string scope;
    double percent = util::NaN;
    double resetsAt = util::NaN;
    double windowMin = util::NaN;   // set when the provider states its own cycle (a billing month)
};

struct Tokens {
    double input = 0, output = 0, cache_write = 0, cache_read = 0, requests = 0;
};

struct Sample {
    double t = 0;
    std::string acct;   // empty = a sample from before accounts were tracked
    std::vector<Limit> limits;
    Ordered<Tokens> tokens;
};
using Samples = std::vector<const Sample*>;

Limit limitFromJson(const json::Value& v);
std::vector<Limit> limitsFromJson(const json::Value& v);
json::Value toJson(const Limit& l);
json::Value toJson(const std::vector<Limit>& ls);
json::Value toJson(const Ordered<Tokens>& t);
json::Value toJson(const Sample& s);
bool sampleFromJson(const json::Value& v, Sample& s);

struct Window {
    std::string key, kind, scope;
    double percent = 0;
    bool isLong = false;
    double elapsed = 0;
    double minutesLeft = util::NaN;
    double speed = 0, rate = 0, projected = 0;
    double allowedPerHour = util::NaN;
    double resetsAt = util::NaN;
    bool idle = false;
};
json::Value toJson(const Window& w);
json::Value toJson(const std::vector<Window>& ws);

std::string keyOf(const Limit& l);
std::vector<Window> windows(const std::vector<Limit>& limits, const Samples& samples, double now);
/// A reset that has passed since the reading zeroes that window instead of freezing it at the old figure.
/// Keep in step with `rolled` in the tray, which applies the same rule between two ticks.
std::vector<Limit> rolled(std::vector<Limit> limits, double now);
/// Observed resets: a drop of more than 5 points between consecutive samples.
Ordered<std::vector<double>> observedResets(const Samples& samples);

struct Fit {
    std::vector<std::string> models;
    int n = 0;
    bool enough = false;
    std::string why;
    Ordered<double> weights, relative;
    double residual = 0;
    double cacheReadPrior = 0.1;
};
json::Value toJson(const Fit& f);
std::optional<std::vector<double>> nnls(const std::vector<std::vector<double>>& X, const std::vector<double>& y, int iters = 20000);
Fit fit(const Samples& samples, double cacheReadPrior = 0.1);

std::string label(const Window& w);
/// One sentence: what to change to land at 100% of the worst window, or nothing when on pace.
std::optional<std::string> advise(const std::vector<Window>& ws, const Samples& samples, const Fit* fitted, double now);

struct Cost {
    double total = 0;
    Ordered<double> byModel;
};
Cost costOf(const Ordered<Tokens>& tokens);
json::Value toJson(const Cost& c);
Ordered<Tokens> spendSince(const Samples& samples, double ms, double now);

/// THE pace verdict — the top level and every account take it from here: the worst window's projected % at
/// its reset, and a window already used up is red whatever its speed.
struct Verdict {
    std::optional<double> pace;
    std::string level = "unknown";
    const Window* worst = nullptr;
};
Verdict verdict(const std::vector<Window>& ws);
/// For a subscription you are not on: is it worth switching back, and if not, when.
std::string standby(const std::vector<Window>& ws);

json::Value buildStatus(const std::vector<Limit>& limits, const Samples& all, double now, const std::string& acctId);

// ---------------------------------------------------------------- accounts
// Every subscription seen on this machine keeps its last reading, so the one you are NOT logged into still
// shows where you left it and when it comes back.
void rememberAccount(json::Value& accounts, const json::Value& acct, double now);
/// id → the history that gives a live login its rate (one live login per provider).
using Live = std::map<std::string, Samples>;
json::Value accountsView(const json::Value& accounts, const std::string& currentId, const Samples& samples, double now, const Live& live,
                         const std::string& firstId);

}  // namespace pace

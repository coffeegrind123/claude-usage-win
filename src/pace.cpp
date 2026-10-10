#include "pace.h"

#include <algorithm>
#include <cmath>

namespace pace {

namespace {

using util::jsNum;
using util::jsRound;

double windowMinOf(const std::string& kind) {
    if (kind == "session") return 5 * 60;
    if (kind == "weekly_all" || kind == "weekly_scoped") return 7 * 24 * 60;
    if (kind == "daily") return 24 * 60;
    if (kind == "monthly") return 30 * 24 * 60;
    return 0;   // unknown: falsy, like WINDOW_MIN[kind] being undefined
}

// Token classes, and the API-price ratios used ONLY as the prior for how classes relate within a model
// (input 1, output 5, cache write 1.25, cache read 0.1). The per-model weight is what gets fitted.
constexpr double CLASS_INPUT = 1, CLASS_OUTPUT = 5, CLASS_CACHE_WRITE = 1.25, CLASS_CACHE_READ = 0.1;

// Price-ratio priors per model relative to sonnet, used until the fit has enough trustworthy data.
const std::map<std::string, double>& priorWeight() {
    static const std::map<std::string, double> w = {{"haiku", 0.3}, {"sonnet", 1}, {"opus", 5}, {"fable", 10}, {"mythos", 10}, {"other", 5}};
    return w;
}

// The weekly percent is an INTEGER, so a 10-minute delta is 0 or 1 and a per-sample regression fits
// quantisation noise (34 samples once "measured" Fable at 0.22× Sonnet). Samples are therefore accumulated
// into bins that each span ≥ FIT_BIN_PCT points before fitting, and a fit is only trusted when it has
// ≥ FIT_MIN_BINS bins and every weight lands within PLAUSIBLE× of the prior.
constexpr double FIT_BIN_PCT = 3;
constexpr int FIT_MIN_BINS = 40;
constexpr double PLAUSIBLE = 4;

// First-party API list prices, USD per 1M tokens (input, output); cache write = 1.25× input, cache read = 0.1× input.
std::pair<double, double> price(const std::string& m) {
    static const std::map<std::string, std::pair<double, double>> p = {
        {"fable", {10, 50}}, {"mythos", {10, 50}}, {"opus", {5, 25}}, {"sonnet", {3, 15}}, {"haiku", {1, 5}}, {"other", {5, 25}}};
    auto it = p.find(m);
    return it != p.end() ? it->second : p.at("other");
}

const char* modelName(const std::string& m) {
    if (m == "haiku") return "Haiku";
    if (m == "sonnet") return "Sonnet";
    if (m == "opus") return "Opus";
    if (m == "fable") return "Fable";
    if (m == "mythos") return "Mythos";
    return "other models";
}

double units(const Tokens& t, double cacheReadPrior = CLASS_CACHE_READ) {
    return (t.input * CLASS_INPUT + t.output * CLASS_OUTPUT + t.cache_write * CLASS_CACHE_WRITE + t.cache_read * cacheReadPrior) / 1e6;
}

const Limit* findLimit(const std::vector<Limit>& ls, const std::string& kind, const std::string& scope) {
    for (const Limit& m : ls) {
        if (m.kind == kind && m.scope == scope) {
            return &m;
        }
    }
    return nullptr;
}

double round2(double v) { return jsRound(v * 100) / 100; }

std::string stripArticle(std::string s) {
    if (util::startsWith(s, "this ")) {
        return s.substr(5);
    }
    if (util::startsWith(s, "the ")) {
        return s.substr(4);
    }
    return s;
}

json::Value optStr(const std::string& s) { return s.empty() ? json::Value() : json::Value(s); }

}  // namespace

// ---------------------------------------------------------------- shapes
Limit limitFromJson(const json::Value& v) {
    Limit l;
    l.kind = v["kind"].str();
    l.scope = v["scope"].str();
    l.percent = v["percent"].isNum() ? v["percent"].num() : util::NaN;
    l.resetsAt = v["resetsAt"].isNum() ? v["resetsAt"].num() : util::NaN;
    l.windowMin = v["windowMin"].isNum() ? v["windowMin"].num() : util::NaN;
    return l;
}

std::vector<Limit> limitsFromJson(const json::Value& v) {
    std::vector<Limit> out;
    for (const auto& x : v.items()) {
        out.push_back(limitFromJson(x));
    }
    return out;
}

json::Value toJson(const Limit& l) {
    json::Value o = json::Value::object();
    o.set("kind", l.kind);
    o.set("scope", optStr(l.scope));
    o.set("percent", std::isfinite(l.percent) ? json::Value(l.percent) : json::Value());
    o.set("resetsAt", std::isfinite(l.resetsAt) ? json::Value(l.resetsAt) : json::Value());
    if (std::isfinite(l.windowMin) && l.windowMin != 0) {
        o.set("windowMin", l.windowMin);
    }
    return o;
}

json::Value toJson(const std::vector<Limit>& ls) {
    json::Value a = json::Value::array();
    for (const auto& l : ls) {
        a.push(toJson(l));
    }
    return a;
}

json::Value toJson(const Ordered<Tokens>& t) {
    json::Value o = json::Value::object();
    for (const auto& [m, r] : t.items) {
        json::Value row = json::Value::object();
        row.set("input", r.input);
        row.set("output", r.output);
        row.set("cache_write", r.cache_write);
        row.set("cache_read", r.cache_read);
        row.set("requests", r.requests);
        o.set(m, std::move(row));
    }
    return o;
}

json::Value toJson(const Sample& s) {
    json::Value o = json::Value::object();
    o.set("t", s.t);
    if (!s.acct.empty()) {
        o.set("acct", s.acct);
    }
    o.set("limits", toJson(s.limits));
    o.set("tokens", toJson(s.tokens));
    return o;
}

bool sampleFromJson(const json::Value& v, Sample& s) {
    if (!v.isObj()) {
        return false;
    }
    s.t = v["t"].num(0);
    s.acct = v["acct"].str();
    s.limits = limitsFromJson(v["limits"]);
    s.tokens.items.clear();
    for (const auto& [m, r] : v["tokens"].members()) {
        if (!r.truthy()) {
            continue;
        }
        Tokens t;
        t.input = r["input"].num(0);
        t.output = r["output"].num(0);
        t.cache_write = r["cache_write"].num(0);
        t.cache_read = r["cache_read"].num(0);
        t.requests = r["requests"].num(0);
        s.tokens.items.emplace_back(m, t);
    }
    return true;
}

json::Value toJson(const Window& w) {
    json::Value o = json::Value::object();
    o.set("key", w.key);
    o.set("kind", w.kind);
    o.set("scope", optStr(w.scope));
    o.set("percent", w.percent);
    o.set("long", w.isLong);
    o.set("elapsed", w.elapsed);
    o.set("minutesLeft", std::isfinite(w.minutesLeft) ? json::Value(w.minutesLeft) : json::Value());
    o.set("speed", w.speed);
    o.set("rate", w.rate);
    o.set("projected", w.projected);
    o.set("allowedPerHour", std::isfinite(w.allowedPerHour) ? json::Value(w.allowedPerHour) : json::Value());
    o.set("resetsAt", std::isfinite(w.resetsAt) ? json::Value(w.resetsAt) : json::Value());
    if (w.idle) {
        o.set("idle", true);
    }
    return o;
}

json::Value toJson(const std::vector<Window>& ws) {
    json::Value a = json::Value::array();
    for (const auto& w : ws) {
        a.push(toJson(w));
    }
    return a;
}

// ---------------------------------------------------------------- pace
std::string keyOf(const Limit& l) { return l.kind + (l.scope.empty() ? "" : ":" + l.scope); }

std::vector<Window> windows(const std::vector<Limit>& limits, const Samples& samples, double now) {
    std::vector<Window> out;
    for (const Limit& l : limits) {
        Window w;
        w.key = keyOf(l);
        w.kind = l.kind;
        w.scope = l.scope;
        double percent = std::isfinite(l.percent) ? l.percent : 0;
        w.percent = percent;
        // An idle window (no usage yet) has resets_at: null. No reset time = the window hasn't started; it is
        // idle by definition: speed 0, projected = used. Letting the NaN through once poisoned everything
        // downstream (pace "—", advice "ease off for the next NaNh").
        if (!std::isfinite(l.resetsAt)) {
            double wm = windowMinOf(l.kind);
            w.isLong = (wm ? wm : 300) > LONG_WINDOW_MIN;
            w.projected = percent;
            w.idle = true;
            out.push_back(w);
            continue;
        }
        double minutesLeft = std::max(1.0, (l.resetsAt - now) / 60000);
        double wm = windowMinOf(l.kind);
        // A provider that states its own cycle (a billing month) wins.
        double length = (std::isfinite(l.windowMin) && l.windowMin != 0) ? l.windowMin : wm ? wm : std::max(minutesLeft, 5.0 * 60);
        bool isLong = length > LONG_WINDOW_MIN;
        double elapsed = std::min(1.0, std::max(0.0, 1 - minutesLeft / length));

        // Short window: rate over the last 45 min projected to the reset.
        double rate = 0;
        for (const Sample* s : samples) {
            if (now - s->t > RATE_WINDOW_MIN * 60000) {
                continue;
            }
            const Limit* m = findLimit(s->limits, l.kind, l.scope);
            if (!m || !std::isfinite(m->percent)) {
                continue;
            }
            if (m->percent <= percent) {
                if (now - s->t > 5 * 60000) {
                    rate = (percent - m->percent) / ((now - s->t) / 60000);
                }
                break;   // the FIRST such sample decides, as `hist.find` does
            }
        }
        double elapsedMin = std::max(length - minutesLeft, MIN_ELAPSED * length);
        double speed = percent / elapsedMin * 60;
        // A wrapper's allowance opens on first use, so its first reading is always "a few % at ~0% elapsed" —
        // which extrapolates to hundreds. Until a twentieth of that window has passed there is no speed yet.
        bool tooEarly = l.kind == "quota" && elapsed < 0.05;
        double projected = tooEarly ? percent : isLong ? percent / std::max(elapsed, MIN_ELAPSED) : percent + std::max(0.0, rate) * minutesLeft;
        // What you may spend per hour from here to land exactly on TARGET.
        double allowedPerHour = std::max(0.0, TARGET - percent) / minutesLeft * 60;

        w.isLong = isLong;
        w.elapsed = elapsed;
        w.minutesLeft = minutesLeft;
        w.speed = speed;
        w.rate = rate * 60;
        w.projected = std::isfinite(projected) ? projected : percent;
        w.allowedPerHour = allowedPerHour;
        w.resetsAt = l.resetsAt;
        out.push_back(w);
    }
    return out;
}

std::vector<Limit> rolled(std::vector<Limit> limits, double now) {
    for (Limit& l : limits) {
        if (std::isfinite(l.resetsAt) && l.resetsAt <= now) {
            l.percent = 0;
            l.resetsAt = util::NaN;
        }
    }
    return limits;
}

Ordered<std::vector<double>> observedResets(const Samples& samples) {
    Ordered<std::vector<double>> out;
    for (size_t i = 1; i < samples.size(); i++) {
        for (const Limit& l : samples[i]->limits) {
            const Limit* prev = findLimit(samples[i - 1]->limits, l.kind, l.scope);
            if (prev && l.percent < prev->percent - 5) {
                out.at(keyOf(l)).push_back(samples[i]->t);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- fit
// Δpercent(weekly_all) = Σ_model w_model × units_model, units = Σ_class prior_class × tokens.
// Non-negative least squares by projected gradient — tiny problem, no library.
std::optional<std::vector<double>> nnls(const std::vector<std::vector<double>>& X, const std::vector<double>& y, int iters) {
    size_t n = X.empty() ? 0 : X[0].size();
    if (!n) {
        return std::nullopt;
    }
    std::vector<double> w(n, 0), diag(n, 0), grad(n);
    for (const auto& row : X) {
        for (size_t j = 0; j < n; j++) {
            diag[j] += row[j] * row[j];
        }
    }
    // Lipschitz step from the Gram diagonal.
    double denom = *std::max_element(diag.begin(), diag.end()) * static_cast<double>(n);
    double step = 1 / (denom ? denom : 1);
    for (int it = 0; it < iters; it++) {
        std::fill(grad.begin(), grad.end(), 0.0);
        for (size_t i = 0; i < X.size(); i++) {
            double r = -y[i];
            for (size_t j = 0; j < n; j++) {
                r += X[i][j] * w[j];
            }
            for (size_t j = 0; j < n; j++) {
                grad[j] += 2 * r * X[i][j];
            }
        }
        double moved = 0;
        for (size_t j = 0; j < n; j++) {
            double nw = std::max(0.0, w[j] - step * grad[j]);
            moved += std::abs(nw - w[j]);
            w[j] = nw;
        }
        if (moved < 1e-12) {
            break;
        }
    }
    return w;
}

Fit fit(const Samples& samples, double cacheReadPrior) {
    Fit f;
    f.cacheReadPrior = cacheReadPrior;
    for (const char* m : {"sonnet", "opus", "fable", "haiku", "other"}) {
        for (const Sample* s : samples) {
            if (s->tokens.find(m)) {
                f.models.push_back(m);
                break;
            }
        }
    }
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    bool open = false;
    double start = 0;
    std::vector<double> row;
    for (size_t i = 1; i < samples.size(); i++) {
        const Limit* cur = nullptr;
        const Limit* prev = nullptr;
        for (const Limit& l : samples[i]->limits) {
            if (l.kind == "weekly_all") { cur = &l; break; }
        }
        for (const Limit& l : samples[i - 1]->limits) {
            if (l.kind == "weekly_all") { prev = &l; break; }
        }
        if (!cur || !prev || cur->percent < prev->percent) {
            open = false;   // reset: drop the open bin
            continue;
        }
        if (!open) {
            open = true;
            start = prev->percent;
            row.assign(f.models.size(), 0);
        }
        for (size_t j = 0; j < f.models.size(); j++) {
            if (const Tokens* t = samples[i]->tokens.find(f.models[j])) {
                row[j] += units(*t, cacheReadPrior);
            }
        }
        if (cur->percent - start >= FIT_BIN_PCT) {
            X.push_back(row);
            y.push_back(cur->percent - start);
            open = false;
        }
    }
    f.n = static_cast<int>(X.size());
    auto has = [&](const char* m) { return std::find(f.models.begin(), f.models.end(), m) != f.models.end(); };
    if (!has("sonnet") || f.models.size() < 2 || f.n < FIT_MIN_BINS) {
        return f;
    }
    std::vector<double> w = *nnls(X, y);
    size_t si = static_cast<size_t>(std::find(f.models.begin(), f.models.end(), "sonnet") - f.models.begin());
    if (!(w[si] > 0)) {
        f.why = "sonnet weight 0";
        return f;
    }
    for (size_t j = 0; j < f.models.size(); j++) {
        const std::string& m = f.models[j];
        if (m == "other" || !(w[j] > 0)) {
            continue;
        }
        double rel = w[j] / w[si];
        auto it = priorWeight().find(m);
        double prior = it != priorWeight().end() ? it->second : 5;
        if (rel > prior * PLAUSIBLE || rel < prior / PLAUSIBLE) {
            f.why = m + " " + util::fixed(rel, 2) + "× implausible vs prior " + jsNum(prior) + "×";
            return f;
        }
    }
    double resid = 0;
    for (size_t i = 0; i < X.size(); i++) {
        double r = -y[i];
        for (size_t j = 0; j < w.size(); j++) {
            r += X[i][j] * w[j];
        }
        resid += r * r;
    }
    double base = w[si];
    f.enough = true;
    for (size_t j = 0; j < f.models.size(); j++) {
        f.weights.at(f.models[j]) = w[j];
        f.relative.at(f.models[j]) = w[j] / base;
    }
    f.residual = resid;
    return f;
}

json::Value toJson(const Fit& f) {
    json::Value o = json::Value::object();
    json::Value models = json::Value::array();
    for (const auto& m : f.models) {
        models.push(m);
    }
    o.set("models", std::move(models));
    o.set("n", f.n);
    o.set("enough", f.enough);
    if (!f.enough) {
        if (!f.why.empty()) {
            o.set("why", f.why);
        }
        return o;
    }
    json::Value weights = json::Value::object(), relative = json::Value::object();
    for (const auto& [m, v] : f.weights.items) {
        weights.set(m, v);
    }
    for (const auto& [m, v] : f.relative.items) {
        relative.set(m, v);
    }
    o.set("weights", std::move(weights));
    o.set("relative", std::move(relative));
    o.set("residual", f.residual);
    o.set("cacheReadPrior", f.cacheReadPrior);
    return o;
}

// ---------------------------------------------------------------- advice
std::string label(const Window& w) {
    if (w.kind == "session") return "this session";
    if (w.kind == "weekly_all") return "the week";
    if (w.kind == "monthly") return "the month";
    if (w.kind == "quota") return "the " + w.scope + " allowance";
    if (w.kind == "daily") {
        std::string s = "today's " + w.scope + " quota";
        size_t p = s.find("  ");
        if (p != std::string::npos) {
            s.replace(p, 2, " ");
        }
        return s;
    }
    return "the " + w.scope + " week";
}

std::optional<std::string> advise(const std::vector<Window>& ws, const Samples& samples, const Fit* fitted, double now) {
    const Window* worst = nullptr;
    for (const Window& w : ws) {
        if (std::isfinite(w.projected) && !w.idle && (!worst || w.projected > worst->projected)) {
            worst = &w;
        }
    }
    if (!worst || worst->projected <= 100) {
        return std::nullopt;
    }
    double remaining = 100 - worst->percent;
    if (remaining <= 0) {
        return label(*worst) + " is used up — wait " + jsNum(jsRound(worst->minutesLeft / 60)) + "h for the reset";
    }
    double keep = remaining / (worst->projected - worst->percent);   // fraction of current speed that still fits
    double cut = 1 - keep;
    std::string cutPct = jsNum(jsRound(cut * 100));
    if (!worst->isLong) {
        return std::isfinite(worst->minutesLeft)
                   ? "this session is burning fast — ease off for the next " + jsNum(jsRound(worst->minutesLeft / 60 * 10) / 10) + "h (about " + cutPct + "% less)"
                   : "this session is burning fast — ease off (about " + cutPct + "% less)";
    }
    std::map<std::string, double> weight = priorWeight();
    if (fitted && fitted->enough) {
        for (const auto& [m, v] : fitted->relative.items) {
            weight[m] = v;
        }
    }
    auto weightOf = [&](const std::string& m) {
        auto it = weight.find(m);
        return it != weight.end() ? it->second : 5.0;
    };
    // Weighted spend share per model across this window.
    Ordered<double> share;
    double total = 0;
    double wm = windowMinOf(worst->kind);
    for (const Sample* s : samples) {
        if (s->t < now - (wm ? wm : 10080) * 60000) {
            continue;
        }
        for (const auto& [m, t] : s->tokens.items) {
            double u = units(t) * weightOf(m);
            share.at(m) += u;
            total += u;
        }
    }
    if (!total) {
        return "spend needs to drop ~" + cutPct + "% to fit " + label(*worst) + " — no per-model data yet";
    }
    for (auto& [m, v] : share.items) {
        v /= total;
    }
    if (worst->kind == "weekly_scoped") {
        return "use " + worst->scope + " about " + cutPct + "% less (it has its own weekly limit)";
    }
    auto shareOf = [&](const char* m) {
        const double* v = share.find(m);
        return v ? *v : 0.0;
    };
    std::vector<std::string> steps;
    double need = cut;
    for (const char* heavy : {"fable", "mythos"}) {
        if (need <= 0 || !shareOf(heavy)) {
            continue;
        }
        double saving = std::max(0.0, shareOf(heavy) * (1 - weightOf("opus") / weightOf(heavy)));   // moving that work to Opus
        if (saving > 0 && saving >= need) {
            steps.push_back(std::string("use ") + modelName(heavy) + " about " + jsNum(jsRound(need / saving * 100)) + "% less");
            need = 0;
        } else if (saving > 0) {
            steps.push_back(std::string("stop using ") + modelName(heavy));
            need -= saving;
        }
    }
    if (need > 0 && shareOf("opus")) {
        double saving = std::max(0.0, shareOf("opus") * (1 - weightOf("sonnet") / weightOf("opus")));   // moving Opus work to Sonnet
        if (saving > 0 && saving >= need) {
            steps.push_back("move about " + jsNum(jsRound(need / saving * 100)) + "% of Opus work to Sonnet");
            need = 0;
        } else if (saving > 0) {
            steps.push_back("move all Opus work to Sonnet");
            need -= saving;
        }
    }
    if (need > 0) {
        steps.push_back(std::string(steps.empty() ? "" : "and still ") + "do about " + jsNum(std::min(100.0, jsRound(need * 100))) + "% less overall");
    }
    std::string joined;
    for (size_t i = 0; i < steps.size(); i++) {
        joined += (i ? ", " : "") + steps[i];
    }
    return "to fit " + label(*worst) + ": " + joined;
}

// ---------------------------------------------------------------- cost
Cost costOf(const Ordered<Tokens>& tokens) {
    Cost c;
    double total = 0;
    for (const auto& [m, t] : tokens.items) {
        auto [pin, pout] = price(m);
        double usd = (t.input * pin + t.cache_write * pin * 1.25 + t.cache_read * pin * 0.1 + t.output * pout) / 1e6;
        c.byModel.at(m) = round2(usd);
        total += usd;
    }
    c.total = round2(total);
    return c;
}

json::Value toJson(const Cost& c) {
    json::Value o = json::Value::object(), by = json::Value::object();
    o.set("total", c.total);
    for (const auto& [m, v] : c.byModel.items) {
        by.set(m, v);
    }
    o.set("byModel", std::move(by));
    return o;
}

Ordered<Tokens> spendSince(const Samples& samples, double ms, double now) {
    Ordered<Tokens> out;
    for (const Sample* s : samples) {
        if (now - s->t > ms) {
            continue;
        }
        for (const auto& [m, t] : s->tokens.items) {
            Tokens& row = out.at(m);
            row.input += t.input;
            row.output += t.output;
            row.cache_write += t.cache_write;
            row.cache_read += t.cache_read;
            row.requests += t.requests;
        }
    }
    return out;
}

// ---------------------------------------------------------------- verdict
Verdict verdict(const std::vector<Window>& ws) {
    Verdict v;
    for (const Window& w : ws) {
        if (std::isfinite(w.projected) && (!v.worst || w.projected > v.worst->projected)) {
            v.worst = &w;
        }
    }
    if (!v.worst) {
        return v;
    }
    v.pace = jsRound(v.worst->projected);
    bool usedUp = std::any_of(ws.begin(), ws.end(), [](const Window& w) { return w.percent >= 100; });
    v.level = *v.pace > 100 || usedUp ? "red" : *v.pace > TARGET ? "amber" : "green";
    return v;
}

std::string standby(const std::vector<Window>& ws) {
    std::vector<const Window*> full;
    for (const Window& w : ws) {
        if (w.percent >= 90 && std::isfinite(w.minutesLeft) && w.minutesLeft) {
            full.push_back(&w);
        }
    }
    if (full.empty()) {
        const Window* worst = nullptr;   // not every provider has a week
        for (const Window& w : ws) {
            if (!worst || w.percent > worst->percent) {
                worst = &w;
            }
        }
        return "ready to switch back — " + (!worst || !worst->percent ? std::string("everything has reset") : stripArticle(label(*worst)) + " at " + jsNum(worst->percent) + "%");
    }
    const Window* wait = full[0];
    for (const Window* w : full) {
        if (w->minutesLeft > wait->minutesLeft) {
            wait = w;
        }
    }
    long long m = static_cast<long long>(jsRound(wait->minutesLeft)), d = m / 1440, h = m % 1440 / 60;
    std::string names;
    for (size_t i = 0; i < full.size(); i++) {
        names += (i ? " and " : "") + stripArticle(label(*full[i]));
    }
    std::string back = d ? std::to_string(d) + "d " + std::to_string(h) + "h" : h ? std::to_string(h) + "h " + std::to_string(m % 60) + "m" : std::to_string(m) + "m";
    return names + " nearly used — back in " + back;
}

// ---------------------------------------------------------------- status
json::Value buildStatus(const std::vector<Limit>& limits, const Samples& all, double now, const std::string& acctId) {
    // Another subscription's samples are not this one's history: its percent is a different counter.
    Samples samples;
    for (const Sample* s : all) {
        if (s->acct.empty() || acctId.empty() || s->acct == acctId) {
            samples.push_back(s);
        }
    }
    // Rolled like every account's windows are: a passed reset the server has not turned over yet reads as idle
    // here too, so the top level cannot say 95% while the same login's tab says 0%.
    std::vector<Window> ws = windows(rolled(limits, now), samples, now);
    Verdict v = verdict(ws);
    Fit fitted = fit(samples);
    Fit fitCache1 = fit(samples, 1);
    std::optional<std::string> advice = advise(ws, samples, &fitted, now);

    json::Value o = json::Value::object();
    o.set("t", now);
    o.set("acct", optStr(acctId));
    o.set("pace", v.pace ? json::Value(*v.pace) : json::Value());
    o.set("level", v.level);
    o.set("worst", v.worst ? json::Value(v.worst->key) : json::Value());
    o.set("target", TARGET);
    o.set("advice", advice ? json::Value(*advice) : json::Value());
    o.set("windows", toJson(ws));
    json::Value resets = json::Value::object();
    for (const auto& [k, ts] : observedResets(samples).items) {
        json::Value arr = json::Value::array();
        for (double t : ts) {
            arr.push(t);
        }
        resets.set(k, std::move(arr));
    }
    o.set("resets", std::move(resets));
    o.set("spend24h", toJson(spendSince(samples, 24 * 3600000.0, now)));
    // What the same tokens would have cost on the API over rolling 1/7/30 days. Read from ALL samples, not this
    // login's: transcripts are shared between profiles, so the tokens were never one account's.
    json::Value cost = json::Value::object();
    cost.set("day", toJson(costOf(spendSince(all, DAY_MS, now))));
    cost.set("week", toJson(costOf(spendSince(all, 7 * DAY_MS, now))));
    cost.set("month", toJson(costOf(spendSince(all, 30 * DAY_MS, now))));
    cost.set("since", all.empty() || !all.front()->t ? now : all.front()->t);
    o.set("cost", std::move(cost));
    o.set("fit", toJson(fitted));
    // Same fit with cache reads at full price: whichever residual is lower says how the limit treats cache.
    if (fitCache1.enough) {
        json::Value fc = json::Value::object();
        fc.set("residual", fitCache1.residual);
        json::Value rel = json::Value::object();
        for (const auto& [m, x] : fitCache1.relative.items) {
            rel.set(m, x);
        }
        fc.set("relative", std::move(rel));
        o.set("fitCacheFull", std::move(fc));
    } else {
        o.set("fitCacheFull", json::Value());
    }
    o.set("samples", static_cast<double>(samples.size()));
    return o;
}

// ---------------------------------------------------------------- accounts
void rememberAccount(json::Value& accounts, const json::Value& acct, double now) {
    std::string id = acct["id"].str();
    bool fresh = !accounts.has(id);
    json::Value merged = fresh ? json::Value::object() : accounts[id];
    for (const auto& [k, v] : acct.members()) {
        merged.set(k, v);
    }
    merged.set("asOf", now);
    accounts.set(id, std::move(merged));
    // A reading recovered from before accounts were tracked is the same subscription if its week ends at the same moment.
    auto week = [](const json::Value& limits) {
        for (const auto& l : limits.items()) {
            if (l["kind"].str() == "weekly_all") {
                return l["resetsAt"].isNum() ? l["resetsAt"].num() : util::NaN;
            }
        }
        return util::NaN;
    };
    if (fresh && acct["provider"].str() == "claude" && accounts.has("legacy") && std::abs(week(accounts["legacy"]["limits"]) - week(acct["limits"])) < 120000) {
        accounts.erase("legacy");
    }
    auto& ms = accounts.members();
    ms.erase(std::remove_if(ms.begin(), ms.end(), [&](const json::Value::Member& m) { return m.second["asOf"].isNum() && now - m.second["asOf"].num() > FORGET_AFTER; }),
             ms.end());
}

json::Value accountsView(const json::Value& accounts, const std::string& currentId, const Samples& samples, double now, const Live& live,
                         const std::string& firstId) {
    static const std::vector<std::string> ORDER = {"claude", "codex", "cursor", "gemini", "antigravity", "copilot"};
    struct Row {
        json::Value v;
        std::string id, provider;
        bool via = false, current = false;
        double asOf = 0;
    };
    std::vector<Row> rows;
    const Samples none;
    for (const auto& [key, a] : accounts.members()) {
        std::string id = a["id"].str();
        bool current = id == currentId || live.count(id);   // one live login per provider
        const Samples& hist = id == currentId ? samples : current ? live.at(id) : none;
        // Not logged in = nothing is being spent: it lands at reset exactly where it stands now.
        std::vector<Window> ws = windows(rolled(limitsFromJson(a["limits"]), now), hist, now);
        if (!current) {
            for (Window& w : ws) {
                w.speed = 0;
                w.rate = 0;
                w.projected = w.percent;
            }
        }
        json::Value advice;
        if (id == currentId) {
            if (auto x = advise(ws, hist, nullptr, now)) {
                advice = *x;
            }
        } else if (current) {
            std::string x = util::replaceAll(advise(ws, hist, nullptr, now).value_or(""), " — no per-model data yet", "");
            if (!x.empty()) {
                advice = x;
            }
        } else {
            advice = standby(ws);
        }
        Verdict v = verdict(ws);
        std::string provider = a["provider"].str();
        std::string vendor = a["vendor"].truthy() ? a["vendor"].str() : provider == "codex" ? "openai" : provider;

        json::Value o = json::Value::object();
        o.set("id", id);
        o.set("provider", provider);
        o.set("vendor", vendor);
        o.set("title", a["title"].truthy() ? a["title"] : json::Value());
        o.set("via", a["via"].truthy() ? a["via"] : json::Value());
        if (a.has("email")) {
            o.set("email", a["email"]);
        }
        if (a.has("plan")) {
            o.set("plan", a["plan"]);
        }
        // `dir` = the CLAUDE_CONFIG_DIR this login was last read from (null = the default); absent = not known yet.
        if (a.has("dir")) {
            o.set("dir", a["dir"]);
        }
        o.set("current", current);
        o.set("asOf", a["asOf"]);
        o.set("windows", toJson(ws));
        o.set("pace", v.pace ? json::Value(*v.pace) : json::Value());
        o.set("level", v.level);
        o.set("advice", std::move(advice));
        rows.push_back({std::move(o), id, provider, a["via"].truthy(), current, a["asOf"].num(0)});
    }
    auto order = [&](const std::string& p) {
        auto it = std::find(ORDER.begin(), ORDER.end(), p);
        return it == ORDER.end() ? -1 : static_cast<int>(it - ORDER.begin());
    };
    // The Claude login used last leads, so it is the tab the tray opens on; your own subscriptions by provider,
    // then what a wrapper resells.
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        bool af = a.id == firstId, bf = b.id == firstId;
        if (af != bf) return af;
        if (a.via != b.via) return !a.via;
        if (order(a.provider) != order(b.provider)) return order(a.provider) < order(b.provider);
        if (a.current != b.current) return a.current;
        return a.asOf > b.asOf;
    });
    json::Value out = json::Value::array();
    for (auto& r : rows) {
        out.push(std::move(r.v));
    }
    return out;
}

}  // namespace pace

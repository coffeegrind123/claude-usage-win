#pragma once
// WinHTTP, one call at a time, plus the retry policy every remote usage read goes through.
#include <string>
#include <utility>
#include <vector>

namespace http {

struct Request {
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct Response {
    int status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

/// One attempt. Throws std::runtime_error on a transport failure, or when `timeoutMs` passes — the whole
/// exchange, not each phase of it: resolve, connect, send and every read share the one deadline.
Response fetch(const std::string& url, const Request& req, double timeoutMs);

// Every remote usage read is one GET that can blip: none of these are public APIs and a 429 or a 504 under
// load is ordinary. Without a retry one blip left the figure stale for a whole 10-minute tick. ONE home for
// the policy, so a provider added later cannot quietly get none of it, and it hands back the Response so each
// caller keeps its own non-OK semantics (Gemini reads a 403 as "personal account, no quota"). Deliberately
// short: a click on the card kicks a tick and waits 45 s for status.json to move, so the whole retry budget
// has to fit inside that. An auth failure is never retried: tokens are never refreshed here, so the 401 would
// be the same 401.
extern std::vector<double> RETRY_MS;   // mutable so the tests can shorten the waits, never the classification
constexpr double ATTEMPT_MS = 15000;
// A PER-CALL budget is not a budget. A tick reads every provider in SEQUENCE, so a per-call allowance is that
// allowance once per provider. So there is ONE deadline for the whole read phase, and it does both jobs: no
// retry sleeps past it, and every attempt's timeout is CLAMPED to what is left of it.
constexpr double READ_BUDGET_MS = 20000;
/// The Antigravity probe runs after the budgeted reads, up to this long per listening port.
constexpr double LOCAL_PROBE_MS = 4000;
/// How long the card waits for status.json to move after a click.
constexpr double CLICK_POLL_MS = 45000;

bool retryable(int status);
void startReadBudget(double now);
/// 0 = no deadline (a direct call, or a test).
void clearReadBudget();
/// What an attempt may spend: its own timeout, or the rest of the run's budget, whichever is shorter.
double attemptMs();
/// Throws the last failure ("<label> 503", a transport error, or "<label>: read budget spent").
Response getWithRetry(const std::string& url, const Request& req, const std::string& label);

}  // namespace http

#pragma once
// Where the readings come from. A provider is one function: (now) → readings, each
//   { acct: { id, provider, email, plan, limits[] }, asOf, readings[] }
// `limits` is the same shape the Claude read produces — kind, scope, percent, resetsAt (+ windowMin when the
// provider states its own cycle) — and everything downstream (accounts, resets, tabs, rings) is shared.
// A provider with no login on this machine returns nothing and costs nothing; one that throws is skipped.
// Only ever talks to a provider's own usage endpoint, with the login that provider already stored, and
// never refreshes or writes a token.
#include <optional>
#include <string>
#include <vector>

#include "json.h"
#include "pace.h"

namespace providers {

// ---------------------------------------------------------------- Claude
/// One Claude Code login: a home's default (~\.claude) or a CLAUDE_CONFIG_DIR profile (~\.claude-*) in it.
/// Windows and Linux Claude Code keep the OAuth credential in `.credentials.json`.
struct Profile {
    std::wstring home;
    int homeIndex = 0;
    std::wstring dir;           // the profile directory; empty for a home's default login
    std::wstring config;        // its .claude.json: "last used" by mtime, and the offline identity
    std::wstring credentials;
    bool isDefault() const { return dir.empty(); }
    /// What status.json reports as `dir`: null for the primary home's default login, else the directory read.
    json::Value dirJson() const;
};
std::vector<Profile> claudeProfiles();

struct Credential {
    std::string accessToken, subscriptionType, rateLimitTier;
};
/// Nothing for a profile nobody has logged into yet; throws for a default login that has no credential.
std::optional<Credential> credential(const Profile& p);
std::vector<pace::Limit> fetchUsage(const Credential& c);

struct Identity {
    std::string id;
    json::Value email, plan;
};
/// Asked of the API with the same token the usage read uses, so the two can never describe different
/// accounts; .claude.json is the offline fallback.
Identity identity(const Credential& c, const Profile& p);

std::string modelFamily(const std::string& model);
/// Tokens per model × class for assistant messages with a timestamp in (since, until], from every home's
/// .claude\projects. De-duplicated on requestId (the last row wins: streaming rows are cumulative).
pace::Ordered<pace::Tokens> scanTokens(double since, double until);

// ---------------------------------------------------------------- the rest
struct Reading {
    json::Value acct;
    double asOf = 0;
    std::vector<pace::Sample> readings;
};
using Provider = std::vector<Reading> (*)(double now);

/// Codex: ~\.codex\auth.json asked live (the endpoint its own /status reads); its session transcripts carry
/// the same figures, so they give the history for a rate — and the whole reading when the live ask fails.
std::vector<Reading> codexReadings(double now, bool live);
std::vector<Reading> cursorReading(double now);
std::vector<Reading> geminiReading(double now);
std::vector<Reading> antigravityReading(double now);
std::vector<Reading> copilotReading(double now);
/// cursor, gemini, antigravity, copilot — the order a tick reads them in.
const std::vector<Provider>& others();

}  // namespace providers

#pragma once
// Win32 plumbing shared by the CLI and the tray: UTF-8 <-> UTF-16, time in the JS sense (epoch ms as a
// double), files, child processes, and the bits of JS formatting the output depends on.
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace util {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

std::wstring wide(std::string_view s);
std::string utf8(std::wstring_view s);

/// Date.now(): integral milliseconds since the Unix epoch.
double nowMs();
/// Date.parse() for the ISO 8601 forms every provider uses. A date-time without an offset is local and a
/// bare date is UTC, as in JS. Any number of fractional-second digits. NaN when it cannot be read.
double parseDate(std::string_view s);
/// Date#toISOString(): "2026-10-10T12:00:00.000Z".
std::string isoString(double ms);
/// Date#toLocaleString(): the user's own short date and time format.
std::string localeString(double ms);
/// strftime over local time ("%b %d, %H:%M").
std::string localFormat(double ms, const char* fmt);

/// Math.round: halves go up, towards +infinity.
inline double jsRound(double x) { return std::floor(x + 0.5); }
/// `${n}` in a template literal.
std::string jsNum(double n);
/// Number#toFixed.
std::string fixed(double n, int digits);
std::string padEnd(std::string s, size_t width);
std::string padStart(std::string s, size_t width);
/// UTF-8-aware length in code points, which is what padEnd counts for the strings the CLI pads.
size_t codepoints(std::string_view s);

std::string lower(std::string s);
bool icontains(std::string_view hay, std::string_view needle);
bool startsWith(std::string_view s, std::string_view prefix);
std::string trim(std::string_view s);
std::vector<std::string_view> splitLines(std::string_view text);
std::string capitalize(const std::string& s);
std::string replaceAll(std::string s, std::string_view from, std::string_view to);

std::string base64UrlDecode(std::string_view s);
/// The claims of a JWT (its middle segment), parsed. Empty object when it is not one.
std::string jwtPayload(std::string_view token);

// ---------------------------------------------------------------- files
std::wstring join(const std::wstring& a, const std::wstring& b);
bool readFile(const std::wstring& path, std::string& out);
/// Write to a sibling temp file, then swap it in, so a reader never sees half a status.json.
bool writeFileAtomic(const std::wstring& path, std::string_view data);
bool appendFile(const std::wstring& path, std::string_view data);
bool exists(const std::wstring& path);
bool isDir(const std::wstring& path);
bool makeDirs(const std::wstring& path);
/// Last write time in epoch ms; NaN when the file cannot be stat'ed.
double mtimeMs(const std::wstring& path);
struct Entry {
    std::wstring name;
    bool dir = false;
    bool link = false;   // a reparse point: a symlink or junction, which a walk does not follow
    double mtime = 0;
};
std::vector<Entry> listDir(const std::wstring& path);
/// Every *.jsonl under `dir`, recursively, not following links (readdirSync's isDirectory() is false for them).
void jsonlFiles(const std::wstring& dir, std::vector<std::wstring>& out);
std::wstring envVar(const wchar_t* name);
std::wstring homeDir();
std::wstring cwd();
std::wstring exePath();

// ---------------------------------------------------------------- processes
struct RunResult {
    bool started = false;
    bool timedOut = false;
    unsigned long exitCode = 0;
    std::string out;
};
/// Runs a command line with no window, stdin and stderr closed, and captures stdout. PATH is searched.
RunResult run(const std::wstring& cmdline, unsigned long timeoutMs);
/// Quotes one argument for CommandLineToArgvW / the MSVC CRT.
std::wstring quoteArg(const std::wstring& arg);

// ---------------------------------------------------------------- output
/// Writes UTF-8 text to stdout or stderr: as UTF-16 to a console, as UTF-8 bytes to a pipe or file.
void out(std::string_view s);
void err(std::string_view s);

}  // namespace util

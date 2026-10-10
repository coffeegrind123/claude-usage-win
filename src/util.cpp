#include "util.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <thread>

#include "json.h"

namespace util {

std::wstring wide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string utf8(std::wstring_view s) {
    if (s.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string o(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), o.data(), n, nullptr, nullptr);
    return o;
}

// ---------------------------------------------------------------- time
namespace {

// 100 ns ticks between 1601-01-01 (FILETIME) and 1970-01-01 (Unix).
constexpr long long EPOCH_DIFF_100NS = 116444736000000000LL;

// Howard Hinnant's civil-calendar algorithms: exact for any proleptic Gregorian date, no CRT time_t limits.
long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

void civilFromDays(long long z, long long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2 ? 1 : 0;
}

bool digits(std::string_view s, size_t& p, size_t n, int& out) {
    if (p + n > s.size()) {
        return false;
    }
    out = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[p + i];
        if (c < '0' || c > '9') {
            return false;
        }
        out = out * 10 + (c - '0');
    }
    p += n;
    return true;
}

bool toSystemTime(double ms, SYSTEMTIME& local) {
    if (!std::isfinite(ms)) {
        return false;
    }
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<ULONGLONG>(static_cast<long long>(ms) * 10000 + EPOCH_DIFF_100NS);
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME utc;
    return FileTimeToSystemTime(&ft, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
}

}  // namespace

double nowMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return static_cast<double>((static_cast<long long>(u.QuadPart) - EPOCH_DIFF_100NS) / 10000);
}

double parseDate(std::string_view s) {
    while (!s.empty() && (s.front() == ' ')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ')) {
        s.remove_suffix(1);
    }
    size_t p = 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, ms = 0;
    if (!digits(s, p, 4, y) || p >= s.size() || s[p++] != '-' || !digits(s, p, 2, mo) || p >= s.size() || s[p++] != '-' || !digits(s, p, 2, d)) {
        return NaN;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) {
        return NaN;
    }
    bool hasTime = false, hasOffset = false;
    int offsetMin = 0;
    if (p < s.size() && (s[p] == 'T' || s[p] == 't' || s[p] == ' ')) {
        p++;
        hasTime = true;
        if (!digits(s, p, 2, h) || p >= s.size() || s[p++] != ':' || !digits(s, p, 2, mi)) {
            return NaN;
        }
        if (p < s.size() && s[p] == ':') {
            p++;
            if (!digits(s, p, 2, sec)) {
                return NaN;
            }
            if (p < s.size() && (s[p] == '.' || s[p] == ',')) {
                p++;
                size_t start = p;
                int scale = 100;
                while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
                    if (scale) {
                        ms += (s[p] - '0') * scale;
                        scale /= 10;
                    }
                    p++;
                }
                if (p == start) {
                    return NaN;
                }
            }
        }
        if (h > 24 || mi > 59 || sec > 59 || (h == 24 && (mi || sec || ms))) {
            return NaN;
        }
        if (p < s.size() && (s[p] == 'Z' || s[p] == 'z')) {
            p++;
            hasOffset = true;
        } else if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
            int sign = s[p++] == '-' ? -1 : 1, oh = 0, om = 0;
            if (!digits(s, p, 2, oh)) {
                return NaN;
            }
            if (p < s.size() && s[p] == ':') {
                p++;
            }
            if (!digits(s, p, 2, om)) {
                return NaN;
            }
            offsetMin = sign * (oh * 60 + om);
            hasOffset = true;
        }
    }
    if (p != s.size()) {
        return NaN;
    }
    if (!hasTime || hasOffset) {
        long long days = daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
        double t = static_cast<double>(days) * 86400000.0 + h * 3600000.0 + mi * 60000.0 + sec * 1000.0 + ms;
        return t - offsetMin * 60000.0;
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == static_cast<time_t>(-1)) {
        return NaN;
    }
    return static_cast<double>(t) * 1000.0 + ms;
}

std::string isoString(double ms) {
    if (!std::isfinite(ms)) {
        return "Invalid Date";
    }
    long long total = static_cast<long long>(std::floor(ms));
    long long days = total >= 0 ? total / 86400000 : -((-total + 86399999) / 86400000);
    long long rem = total - days * 86400000;
    long long y;
    unsigned m, d;
    civilFromDays(days, y, m, d);
    char buf[40];
    snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ", y, m, d, rem / 3600000, rem / 60000 % 60, rem / 1000 % 60, rem % 1000);
    return buf;
}

std::string localeString(double ms) {
    SYSTEMTIME st;
    if (!toSystemTime(ms, st)) {
        return "Invalid Date";
    }
    wchar_t date[80] = {}, time[80] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, 80, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, nullptr, time, 80);
    return utf8(date) + ", " + utf8(time);
}

std::string localFormat(double ms, const char* fmt) {
    if (!std::isfinite(ms)) {
        return "";
    }
    time_t t = static_cast<time_t>(std::floor(ms / 1000));
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) {
        return "";
    }
    char buf[128];
    size_t n = strftime(buf, sizeof buf, fmt, &tm);
    return std::string(buf, n);
}

// ---------------------------------------------------------------- formatting
std::string jsNum(double n) { return json::number(n); }

std::string fixed(double n, int digits) {
    if (!std::isfinite(n)) {
        return json::number(n);
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", digits, n);
    std::string s(buf);
    // toFixed never prints a negative zero.
    if (s[0] == '-' && s.find_first_not_of("-0.") == std::string::npos) {
        s.erase(0, 1);
    }
    return s;
}

size_t codepoints(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

std::string padEnd(std::string s, size_t width) {
    size_t n = codepoints(s);
    if (n < width) {
        s.append(width - n, ' ');
    }
    return s;
}

std::string padStart(std::string s, size_t width) {
    size_t n = codepoints(s);
    if (n < width) {
        s.insert(0, width - n, ' ');
    }
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

bool icontains(std::string_view hay, std::string_view needle) {
    return lower(std::string(hay)).find(lower(std::string(needle))) != std::string::npos;
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) {
        a++;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) {
        b--;
    }
    return std::string(s.substr(a, b - a));
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        size_t end = nl == std::string_view::npos ? text.size() : nl;
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        out.push_back(line);
        if (nl == std::string_view::npos) {
            break;
        }
        start = nl + 1;
    }
    return out;
}

std::string capitalize(const std::string& s) {
    if (s.empty()) {
        return s;
    }
    std::string o = s;
    if (o[0] >= 'a' && o[0] <= 'z') {
        o[0] = static_cast<char>(o[0] - 'a' + 'A');
    }
    return o;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return s;
    }
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}

std::string base64UrlDecode(std::string_view s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (char c : s) {
        int v = val(c);
        if (v < 0) {
            continue;   // padding, or whitespace: Buffer.from(…, 'base64url') skips both
        }
        buf = (buf << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}

std::string jwtPayload(std::string_view token) {
    size_t a = token.find('.');
    if (a == std::string_view::npos) {
        return {};
    }
    size_t b = token.find('.', a + 1);
    return base64UrlDecode(token.substr(a + 1, b == std::string_view::npos ? std::string_view::npos : b - a - 1));
}

// ---------------------------------------------------------------- files
std::wstring join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) {
        return b;
    }
    if (a.back() == L'\\' || a.back() == L'/') {
        return a + b;
    }
    return a + L"\\" + b;
}

bool readFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (1LL << 31)) {
        CloseHandle(h);
        return false;
    }
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD total = 0;
    while (total < out.size()) {
        DWORD got = 0;
        if (!ReadFile(h, out.data() + total, static_cast<DWORD>(out.size() - total), &got, nullptr) || got == 0) {
            break;
        }
        total += got;
    }
    out.resize(total);
    CloseHandle(h);
    return true;
}

bool writeFileAtomic(const std::wstring& path, std::string_view data) {
    std::wstring tmp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD wrote = 0;
    bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) && wrote == data.size();
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    // A reader holding the old file open (the tray, mid-parse) can make the swap fail for a moment.
    for (int i = 0; i < 20; i++) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        Sleep(25);
    }
    DeleteFileW(tmp.c_str());
    return false;
}

bool appendFile(const std::wstring& path, std::string_view data) {
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD wrote = 0;
    bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) && wrote == data.size();
    CloseHandle(h);
    return ok;
}

bool exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool isDir(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool makeDirs(const std::wstring& path) {
    if (path.empty() || isDir(path)) {
        return true;
    }
    size_t cut = path.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 0 && path[cut - 1] != L':') {
        makeDirs(path.substr(0, cut));
    }
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

namespace {
double fileTimeMs(const FILETIME& ft) {
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return static_cast<double>((static_cast<long long>(u.QuadPart) - EPOCH_DIFF_100NS) / 10000);
}
}  // namespace

double mtimeMs(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) {
        return NaN;
    }
    return fileTimeMs(d.ftLastWriteTime);
}

std::vector<Entry> listDir(const std::wstring& path) {
    std::vector<Entry> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(join(path, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        return out;
    }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) {
            continue;
        }
        Entry e;
        e.name = fd.cFileName;
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.mtime = fileTimeMs(fd.ftLastWriteTime);
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

void jsonlFiles(const std::wstring& dir, std::vector<std::wstring>& out) {
    for (const Entry& e : listDir(dir)) {
        std::wstring p = join(dir, e.name);
        if (e.dir) {
            if (!e.link) {
                jsonlFiles(p, out);
            }
        } else if (e.name.size() >= 6 && _wcsicmp(e.name.c_str() + e.name.size() - 6, L".jsonl") == 0) {
            out.push_back(p);
        }
    }
}

std::wstring envVar(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) {
        return {};
    }
    std::wstring v(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(got);
    return v;
}

std::wstring homeDir() {
    std::wstring h = envVar(L"USERPROFILE");
    if (h.empty()) {
        h = envVar(L"HOMEDRIVE") + envVar(L"HOMEPATH");
    }
    return h;
}

std::wstring cwd() {
    DWORD n = GetCurrentDirectoryW(0, nullptr);
    std::wstring s(n, L'\0');
    DWORD got = GetCurrentDirectoryW(n, s.data());
    s.resize(got);
    return s;
}

std::wstring exePath() {
    std::wstring buf(MAX_PATH, L'\0');
    while (true) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

// ---------------------------------------------------------------- processes
std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring o = L"\"";
    for (size_t i = 0;; i++) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            i++;
            slashes++;
        }
        if (i == arg.size()) {
            o.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            o.append(slashes * 2 + 1, L'\\');
        } else {
            o.append(slashes, L'\\');
        }
        o += arg[i];
    }
    o += L'"';
    return o;
}

RunResult run(const std::wstring& cmdline, unsigned long timeoutMs) {
    RunResult r;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        return r;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writeEnd;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }
    if (!ok) {
        CloseHandle(readEnd);
        return r;
    }
    r.started = true;
    std::string out;
    std::thread reader([&] {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(readEnd, buf, sizeof buf, &got, nullptr) && got) {
            out.append(buf, got);
        }
    });
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) {
        r.timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    GetExitCodeProcess(pi.hProcess, &r.exitCode);
    // A grandchild that inherited the pipe could keep it open past the child's exit; cancel the read then.
    if (WaitForSingleObject(reader.native_handle(), 2000) == WAIT_TIMEOUT) {
        CancelSynchronousIo(reader.native_handle());
    }
    reader.join();
    CloseHandle(readEnd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    r.out = std::move(out);
    return r;
}

// ---------------------------------------------------------------- output
namespace {
void put(DWORD which, std::string_view s) {
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode = 0, wrote = 0;
    if (GetConsoleMode(h, &mode)) {
        std::wstring w = wide(s);
        WriteConsoleW(h, w.data(), static_cast<DWORD>(w.size()), &wrote, nullptr);
    } else {
        WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &wrote, nullptr);
    }
}
}  // namespace

void out(std::string_view s) { put(STD_OUTPUT_HANDLE, s); }
void err(std::string_view s) { put(STD_ERROR_HANDLE, s); }

}  // namespace util

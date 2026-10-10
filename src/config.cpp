#include "config.h"

#include <windows.h>

#include "util.h"

namespace config {

namespace {

std::vector<std::wstring> overrides;
std::vector<std::wstring> cached;
bool resolved = false;

std::wstring normalise(const std::wstring& dir) {
    if (dir.empty()) {
        return dir;
    }
    wchar_t buf[4096];
    DWORD n = GetFullPathNameW(dir.c_str(), 4096, buf, nullptr);
    std::wstring out = n && n < 4096 ? std::wstring(buf, n) : dir;
    for (wchar_t& c : out) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    while (out.size() > 3 && out.back() == L'\\') {
        out.pop_back();
    }
    return out;
}

void add(std::vector<std::wstring>& list, const std::wstring& dir) {
    // A pasted path often carries quotes or stray spaces.
    size_t a = dir.find_first_not_of(L" \t\"");
    size_t b = dir.find_last_not_of(L" \t\"");
    std::wstring d = a == std::wstring::npos ? L"" : normalise(dir.substr(a, b - a + 1));
    if (d.empty()) {
        return;
    }
    for (const auto& x : list) {
        if (_wcsicmp(x.c_str(), d.c_str()) == 0) {
            return;
        }
    }
    list.push_back(d);
}

std::vector<std::wstring> splitSemicolons(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find(L';', start);
        out.push_back(s.substr(start, p == std::wstring::npos ? std::wstring::npos : p - start));
        if (p == std::wstring::npos) {
            break;
        }
        start = p + 1;
    }
    return out;
}

}  // namespace

void addHomeOverride(const std::wstring& dir) {
    overrides.push_back(dir);
    resolved = false;
}

bool homesFromEnv() { return !util::envVar(L"CLAUDE_USAGE_HOMES").empty(); }

std::vector<std::wstring> storedHomes() {
    std::vector<std::wstring> out;
    DWORD type = 0, size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Homes", RRF_RT_REG_MULTI_SZ, &type, nullptr, &size) != ERROR_SUCCESS || !size) {
        return out;
    }
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Homes", RRF_RT_REG_MULTI_SZ, &type, buf.data(), &size) != ERROR_SUCCESS) {
        return out;
    }
    for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1) {
        out.emplace_back(p);
    }
    return out;
}

void setStoredHomes(const std::vector<std::wstring>& dirs) {
    std::wstring multi;
    for (const auto& d : dirs) {
        multi += d;
        multi += L'\0';
    }
    multi += L'\0';
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, L"Homes", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(multi.data()), static_cast<DWORD>(multi.size() * sizeof(wchar_t)));
    RegCloseKey(key);
    resolved = false;
}

const std::vector<std::wstring>& homes() {
    if (resolved) {
        return cached;
    }
    std::vector<std::wstring> list;
    for (const auto& d : overrides) {
        add(list, d);
    }
    std::wstring env = util::envVar(L"CLAUDE_USAGE_HOMES");
    if (!env.empty()) {
        for (const auto& d : splitSemicolons(env)) {
            add(list, d);
        }
    } else {
        for (const auto& d : storedHomes()) {
            add(list, d);
        }
        add(list, util::homeDir());
    }
    cached.clear();
    for (const auto& d : list) {
        if (util::isDir(d)) {
            cached.push_back(d);
        }
    }
    // Nothing configured exists (an unmounted share): fall back so the state still has somewhere to live.
    if (cached.empty()) {
        cached.push_back(normalise(util::homeDir()));
    }
    resolved = true;
    return cached;
}

std::wstring stateDir() { return util::join(util::join(homes().front(), L".claude"), L"claude-usage"); }
std::wstring samplesPath() { return util::join(stateDir(), L"samples.jsonl"); }
std::wstring statusPath() { return util::join(stateDir(), L"status.json"); }
std::wstring accountsPath() { return util::join(stateDir(), L"accounts.json"); }
std::wstring remindersPath() { return util::join(stateDir(), L"reminders.json"); }
std::wstring tickLogPath() { return util::join(stateDir(), L"tick.log"); }

}  // namespace config

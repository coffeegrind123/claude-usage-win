// The tray app: the icon (two speed rings for the tab that is open), the card, notifications, the reset poll
// and the failed-read retry. Every 10 minutes and at start the app runs its own executable as `tick` in a
// short-lived hidden child, so the sampling work (every sample, every transcript) never lives in the tray
// process, which stays a few MB. The child writes status.json; the app reads it.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "card.h"
#include "cli.h"
#include "config.h"
#include "draw.h"
#include "pace.h"
#include "reminders.h"
#include "resource.h"
#include "ui_model.h"
#include "util.h"
#include "version.h"

namespace {

constexpr wchar_t WINDOW_CLASS[] = L"ClaudeUsageWin.Tray";
constexpr wchar_t APP_NAME[] = L"Claude Usage";
constexpr wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t RUN_VALUE[] = L"ClaudeUsageWin";
constexpr wchar_t STATE_KEY[] = L"Software\\ClaudeUsageWin\\State";
constexpr UINT TRAY_ID = 1;

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_TICK_DONE = WM_APP + 2;
constexpr UINT WM_SHOW_CARD = WM_APP + 3;   // a second launch asks the running one to open its card

constexpr UINT_PTR TIMER_REFRESH = 1, TIMER_TICK = 2, TIMER_POLL = 3, TIMER_RESET = 4, TIMER_BALLOON = 5;
constexpr UINT REFRESH_MS = 60 * 1000;
constexpr UINT TICK_MS = 600 * 1000;
constexpr UINT POLL_MS = 500;
constexpr double POLL_GIVE_UP_MS = 45 * 1000;  // the click-to-poll wait; the tick's read budget fits inside it
constexpr double TICK_KILL_MS = 120 * 1000;    // a tick that has not finished by now is hung
constexpr UINT BALLOON_FALLBACK_MS = 30 * 1000;
constexpr size_t TICK_LOG_MAX = 1 << 20;
/// A tray click this soon after the card closed is the click that closed it.
constexpr ULONGLONG CLICK_DEBOUNCE_MS = 300;

// Context-menu ids.
enum : UINT {
    ID_SAMPLE = 100,
    ID_STARTUP,
    ID_OPEN_STATE,
    ID_ADD_HOME,
    ID_QUIT,
    ID_REMOVE_HOME = 1000,   // + index into the stored homes
    ID_COPY_REMINDER = 2000, // + index into the reminders
    ID_DONE_REMINDER = 3000,
};

// ---------------------------------------------------------------- persisted flags
HKEY stateKey() {
    static HKEY key = [] {
        HKEY k = nullptr;
        RegCreateKeyExW(HKEY_CURRENT_USER, STATE_KEY, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr);
        return k;
    }();
    return key;
}

bool getBool(const std::string& name) {
    DWORD v = 0, size = sizeof v;
    return RegGetValueW(stateKey(), nullptr, util::wide(name).c_str(), RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v;
}

void setBool(const std::string& name, bool v) {
    DWORD d = v ? 1 : 0;
    RegSetValueExW(stateKey(), util::wide(name).c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&d), sizeof d);
}

bool hasValue(const std::string& name) {
    return RegQueryValueExW(stateKey(), util::wide(name).c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

long long getInt(const std::string& name) {
    long long v = 0;
    DWORD size = sizeof v;
    RegGetValueW(stateKey(), nullptr, util::wide(name).c_str(), RRF_RT_REG_QWORD, nullptr, &v, &size);
    return v;
}

void setInt(const std::string& name, long long v) {
    RegSetValueExW(stateKey(), util::wide(name).c_str(), 0, REG_QWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
}

void removeValue(const std::string& name) { RegDeleteValueW(stateKey(), util::wide(name).c_str()); }

// ---------------------------------------------------------------- start with Windows
std::wstring runCommand() { return util::quoteArg(util::exePath()); }

bool startsWithWindows() {
    wchar_t buf[2048];
    DWORD size = sizeof buf;
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void setStartsWithWindows(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    if (on) {
        std::wstring cmd = runCommand();
        RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, RUN_VALUE);
    }
    RegCloseKey(key);
}

void copyToClipboard(HWND owner, const std::string& text) {
    std::wstring w = util::wide(text);
    if (!OpenClipboard(owner)) {
        return;
    }
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (mem) {
        memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

/// Copies one state file into a new state directory that lacks it — so adding a home does not start the history over.
void carryOver(const std::wstring& fromDir, const std::wstring& toDir, const wchar_t* name) {
    std::wstring from = util::join(fromDir, name), to = util::join(toDir, name);
    if (util::exists(from) && !util::exists(to)) {
        util::makeDirs(toDir);
        CopyFileW(from.c_str(), to.c_str(), TRUE);
    }
}

class App {
public:
    explicit App(std::vector<std::string> homeArgs) : homeArgs_(std::move(homeArgs)) {}

    bool start(HINSTANCE inst) {
        inst_ = inst;
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.lpszClassName = WINDOW_CLASS;
        RegisterClassExW(&wc);
        // A hidden top-level window rather than a message-only one: only top-level windows hear TaskbarCreated
        // (Explorer restarted) and the theme's WM_SETTINGCHANGE broadcast.
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, WINDOW_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        if (!hwnd_ || !card_.create(inst)) {
            return false;
        }
        card_.onPoll = [this] { poll(); };
        card_.onSelect = [this] { refresh(); };   // the icon follows the tab
        card_.onQuit = [this] { quit(); };
        card_.onHidden = [] { trim(); };
        card_.onCopyReminder = [this](const std::string& id) { copyReminder(id); };
        card_.onDoneReminder = [this](const std::string& id) { doneReminder(id); };
        addIcon();
        refresh();
        // RunAtLoad: sample now, then on the interval.
        spawnTick();
        SetTimer(hwnd_, TIMER_REFRESH, REFRESH_MS, nullptr);
        SetTimer(hwnd_, TIMER_TICK, TICK_MS, nullptr);
        return true;
    }

private:
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    Card card_;
    HICON icon_ = nullptr;
    std::vector<std::string> homeArgs_;

    // The tick child.
    HANDLE tickProcess_ = nullptr;
    HANDLE tickWait_ = nullptr;
    double tickStarted_ = 0;

    // Click-to-poll.
    double pollBefore_ = 0;
    double pollStarted_ = 0;
    std::optional<double> armedFor_;
    int errorPolls_ = 0;   // consecutive failed reads already asked about
    double lastErrorPoll_ = 0;

    // Notifications, one balloon at a time.
    struct Note {
        std::string title, body, command;
    };
    std::deque<Note> notes_;
    bool showingNote_ = false;
    std::string shownCommand_;

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) {
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        }
        auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }

    static void trim() {
        // The card's bitmaps and fonts are only needed while it is open: hand the pages back.
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
    }

    // ------------------------------------------------------------ tray icon
    NOTIFYICONDATAW iconData() const {
        NOTIFYICONDATAW nid{sizeof nid};
        nid.hWnd = hwnd_;
        nid.uID = TRAY_ID;
        return nid;
    }

    void addIcon() {
        NOTIFYICONDATAW nid = iconData();
        // Hovering shows the standard tooltip (what the rings mean); the card opens on a click.
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = icon_ ? icon_ : LoadIconW(inst_, MAKEINTRESOURCEW(IDI_APP));
        wcsncpy_s(nid.szTip, APP_NAME, _TRUNCATE);
        Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }

    bool iconRect(RECT& r) const {
        NOTIFYICONIDENTIFIER id{sizeof id};
        id.hWnd = hwnd_;
        id.uID = TRAY_ID;
        return SUCCEEDED(Shell_NotifyIconGetRect(&id, &r));
    }

    RECT anchor() const {
        RECT r;
        if (iconRect(r)) {
            return r;
        }
        POINT pt;   // the icon is in the overflow flyout: open by the pointer instead
        GetCursorPos(&pt);
        return {pt.x, pt.y, pt.x + 1, pt.y + 1};
    }

    int iconSize() const { return GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()); }

    /// Two rings, session then week, for the account whose TAB is open — the tab outlives the card, so the
    /// tray and the card say the same thing rather than the icon silently meaning a different subscription.
    /// (status.json's top level stays pinned to the default Claude login regardless: that is its own contract.)
    void updateIcon(const ui::Status& s) {
        const ui::Account* shown = card_.account();
        // ONE `live`, feeding both the drawing and the words: a remembered login has no speed, so its rings
        // show what was USED on a different ladder, and the tooltip must explain the scale it was drawn on.
        bool live = shown ? shown->current : true;
        HICON next = draw::trayIcon(shown, s, live, iconSize());
        NOTIFYICONDATAW nid = iconData();
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.hIcon = next;
        std::string who;
        if (shown && shown->title) {
            who = *shown->title;
        } else if (const char* pn = ui::providerName(shown ? shown->provider : "claude")) {
            who = pn;
        }
        if (shown && shown->email) {
            who += " · " + *shown->email;
        }
        std::string scale = live ? "session and week speed (projected % at reset; solid red = runs out first)"
                                 : "session and week used at the last check (green = it has reset since; solid red = used up)";
        wcsncpy_s(nid.szTip, util::wide(who + " — " + scale).c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        if (icon_) {
            DestroyIcon(icon_);
        }
        icon_ = next;
    }

    // ------------------------------------------------------------ refresh
    void refresh() {
        ui::Status s = ui::loadStatus();
        bool stale = util::nowMs() - s.t > pace::STALE_AFTER_MS;
        card_.setStatus(s);   // before the icon: it reads the tab, and a tab whose account is gone is reset here
        card_.setReminders(reminders::load());
        updateIcon(card_.status());
        if (!stale) {
            notifyIfNeeded(s);
        }
        notifyDueReminders();
        scheduleResetPoll(s);
        retryFailedRead(s);
        watchTick();
        if (!card_.visible()) {
            trim();
        }
    }

    /// The scheduler fires on a fixed interval, so a tick that failed OUTRIGHT — a 429 outliving its in-tick
    /// retries, the network down — would leave the figure untouched for ten minutes. The app is already re-reading
    /// every 60 s, so it asks again sooner: the FIRST failure at once, then 120 → 240 → 480 → 600 s and never
    /// further, so a persistently failing endpoint is asked no more often than the interval would have.
    void retryFailedRead(const ui::Status& s) {
        if (!s.error) {
            errorPolls_ = 0;
            return;
        }
        if (card_.polling()) {
            return;   // one already in flight: it will refresh us when it lands
        }
        double now = util::nowMs() / 1000;
        double wait = std::min(600.0, 60 * std::pow(2.0, errorPolls_));
        if (now - lastErrorPoll_ < wait) {
            return;
        }
        lastErrorPoll_ = now;
        errorPolls_++;
        poll();
    }

    /// Claude Code learns of a reset from the headers on its next request; the tick only samples every 10
    /// minutes. So the app asks again right after each reset it knows is coming. If the server has not turned
    /// the window over yet, the fresh status still carries that reset and this re-arms.
    void scheduleResetPoll(const ui::Status& s) {
        // refresh() runs every minute: re-arming each time would kill the timer before it is ever due.
        if (s.nextReset == armedFor_) {
            return;
        }
        KillTimer(hwnd_, TIMER_RESET);
        armedFor_ = s.nextReset;
        if (!s.nextReset) {
            return;
        }
        double secs = std::max(5.0, *s.nextReset + 15 - util::nowMs() / 1000);
        SetTimer(hwnd_, TIMER_RESET, static_cast<UINT>(std::min(secs * 1000, static_cast<double>(USER_TIMER_MAXIMUM))), nullptr);
    }

    // ------------------------------------------------------------ the tick
    bool tickRunning() const { return tickProcess_ && WaitForSingleObject(tickProcess_, 0) == WAIT_TIMEOUT; }

    void watchTick() {
        if (tickRunning() && util::nowMs() - tickStarted_ > TICK_KILL_MS) {
            TerminateProcess(tickProcess_, 1);
        }
    }

    static void CALLBACK tickExited(PVOID ctx, BOOLEAN) { PostMessageW(static_cast<HWND>(ctx), WM_TICK_DONE, 0, 0); }

    /// Runs this executable as `tick`, its output appended to tick.log. One at a time: a kick while one runs
    /// is that run.
    void spawnTick() {
        if (tickRunning()) {
            return;
        }
        if (tickWait_) {
            UnregisterWaitEx(tickWait_, nullptr);
            tickWait_ = nullptr;
        }
        if (tickProcess_) {
            CloseHandle(tickProcess_);
            tickProcess_ = nullptr;
        }
        std::wstring log = config::tickLogPath();
        util::makeDirs(config::stateDir());
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &d) && d.nFileSizeLow > TICK_LOG_MAX) {
            MoveFileExW(log.c_str(), (log + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        util::appendFile(log, "--- " + util::isoString(util::nowMs()) + " tick\n");
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE out = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        std::wstring cmd = util::quoteArg(util::exePath());
        for (const auto& h : homeArgs_) {
            cmd += L" --home " + util::quoteArg(util::wide(h));
        }
        cmd += L" tick";
        STARTUPINFOW si{sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = out;
        si.hStdError = out;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            tickProcess_ = pi.hProcess;
            tickStarted_ = util::nowMs();
            RegisterWaitForSingleObject(&tickWait_, tickProcess_, tickExited, hwnd_, INFINITE, WT_EXECUTEONLYONCE);
        }
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    }

    /// Click-to-poll: kick the one tick (never a second sampler) and wait for status.json to move.
    void poll() {
        if (card_.polling()) {
            return;
        }
        card_.setPolling(true);
        pollBefore_ = ui::loadStatus().stamp;
        pollStarted_ = util::nowMs();
        spawnTick();
        SetTimer(hwnd_, TIMER_POLL, POLL_MS, nullptr);
    }

    void checkPoll() {
        if (!card_.polling()) {
            return;
        }
        if (ui::loadStatus().stamp != pollBefore_ || util::nowMs() - pollStarted_ > POLL_GIVE_UP_MS || !tickRunning()) {
            KillTimer(hwnd_, TIMER_POLL);
            card_.setPolling(false);
            refresh();
        }
    }

    // ------------------------------------------------------------ notifications
    void notify(std::string title, std::string body, std::string command = "") {
        notes_.push_back({std::move(title), std::move(body), std::move(command)});
        showNextNote();
    }

    void showNextNote() {
        if (showingNote_ || notes_.empty()) {
            return;
        }
        Note n = notes_.front();
        notes_.pop_front();
        NOTIFYICONDATAW nid = iconData();
        nid.uFlags = NIF_INFO;
        wcsncpy_s(nid.szInfoTitle, util::wide(n.title).c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, util::wide(n.body).c_str(), _TRUNCATE);
        nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
        nid.hBalloonIcon = LoadIconW(inst_, MAKEINTRESOURCEW(IDI_APP));
        if (Shell_NotifyIconW(NIM_MODIFY, &nid)) {
            showingNote_ = true;
            shownCommand_ = n.command;
            SetTimer(hwnd_, TIMER_BALLOON, BALLOON_FALLBACK_MS, nullptr);
        }
    }

    void noteGone(bool clicked) {
        KillTimer(hwnd_, TIMER_BALLOON);
        showingNote_ = false;
        std::string cmd = std::move(shownCommand_);
        shownCommand_.clear();
        if (clicked && !cmd.empty()) {
            copyToClipboard(hwnd_, cmd);
            notes_.push_front({"Copied", "Paste it in a terminal when you are ready.", ""});
        }
        showNextNote();
    }

    // Thresholds, each fired once per crossing: pace goes over 100 / comes back under; a window passes 90% used (once per reset).
    void notifyIfNeeded(const ui::Status& s) {
        // These read s.pace / s.windows — the login `acct` pins the top level to — while the icon follows
        // whichever tab is open. With more than one account the notification has to say whose figure it is.
        const ui::Account* pinned = nullptr;
        for (const auto& a : s.accounts) {
            if (s.acct && a.id == *s.acct) {
                pinned = &a;
            }
        }
        if (!pinned && !s.accounts.empty()) {
            pinned = &s.accounts.front();
        }
        std::string whose;
        if (s.accounts.size() > 1 && pinned) {
            if (pinned->email) whose = " · " + *pinned->email;
            else if (pinned->title) whose = " · " + *pinned->title;
        }
        if (s.pace) {
            int pace = static_cast<int>(*s.pace);
            bool over = pace > 100;
            if (over != getBool("overPace")) {
                setBool("overPace", over);
                if (over) {
                    notify("Off pace: " + std::to_string(pace) + whose, s.advice ? util::capitalize(*s.advice) : "You will run out before the reset");
                } else if (getBool("everOver")) {
                    notify("Back on pace: " + std::to_string(pace) + whose, "Current speed fits the limits again");
                }
            }
            if (over) {
                setBool("everOver", true);
            }
        }
        for (const ui::Win& w : s.windows) {
            // A cycle is identified by the absolute moment it ENDS, never by how long is LEFT: read time plus
            // remaining is constant within a cycle (to seconds of jitter) and jumps by the whole window at a real
            // reset, so 10 minutes of tolerance is far beyond any drift and far short of any turnover.
            if (!w.hoursLeft) {
                continue;   // idle, or reset since the read: nothing to warn about
            }
            std::string key = "warned90:" + w.key, cycleKey = "cycle:" + w.key;
            long long endsAt = ui::cycleId(s.t, *w.hoursLeft);
            if (!hasValue(cycleKey)) {
                setInt(cycleKey, endsAt);
            } else if (std::llabs(getInt(cycleKey) - endsAt) > 600) {
                setBool(key, false);
                setInt(cycleKey, endsAt);
            }
            if (w.percent >= 90 && !getBool(key)) {
                setBool(key, true);
                notify(name(w.key) + " at " + std::to_string(static_cast<int>(w.percent)) + "%" + whose, "Resets in " + ui::countdown(*w.hoursLeft));
            }
        }
    }

    static std::string name(const std::string& key) {
        if (key == "session") return "Session";
        if (key == "weekly_all") return "Week";
        if (util::startsWith(key, "weekly_scoped:")) return key.substr(14) + " week";
        return key;
    }

    // ------------------------------------------------------------ reminders
    /// Each reminder notifies once, when it falls due; the card keeps it until Done. A recurring one moves to
    /// its next date as it fires, so the latch is per occurrence.
    void notifyDueReminders() {
        double now = util::nowMs();
        for (const auto& r : reminders::load()) {
            std::string latch = "reminded:" + r.id + ":" + std::to_string(static_cast<long long>(r.at));
            if (r.at > now || getBool(latch)) {
                continue;
            }
            setBool(latch, true);
            if (!r.every.empty()) {
                if (auto next = reminders::nextDate(r.at, r.every, now)) {
                    reminders::reschedule(r.id, *next);
                }
            }
            notify(r.title + " is due", "Click to copy the command, then paste it in a terminal.", reminders::command(r));
        }
    }

    void copyReminder(const std::string& id) {
        for (const auto& r : reminders::load()) {
            if (r.id == id) {
                copyToClipboard(hwnd_, reminders::command(r));
            }
        }
    }

    void doneReminder(const std::string& id) {
        reminders::remove(id);
        card_.setReminders(reminders::load());
    }

    // ------------------------------------------------------------ the context menu
    void contextMenu() {
        card_.hide();
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ID_SAMPLE, L"Sample now");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        std::vector<reminders::Reminder> rs = reminders::load();
        if (!rs.empty()) {
            HMENU sub = CreatePopupMenu();
            double now = util::nowMs();
            for (size_t i = 0; i < rs.size() && i < 900; i++) {
                HMENU item = CreatePopupMenu();
                AppendMenuW(item, MF_STRING, ID_COPY_REMINDER + i, L"Copy command");
                AppendMenuW(item, MF_STRING, ID_DONE_REMINDER + i, L"Done");
                std::string when = rs[i].at <= now ? "due" : util::localFormat(rs[i].at, "%a %b %#d, %H:%M");
                AppendMenuW(sub, MF_POPUP, reinterpret_cast<UINT_PTR>(item), util::wide(rs[i].title + " — " + when).c_str());
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"Reminders");
        }

        HMENU homes = CreatePopupMenu();
        const auto& active = config::homes();
        for (size_t i = 0; i < active.size(); i++) {
            std::wstring label = active[i] + (i == 0 ? L"   (state lives here)" : L"");
            AppendMenuW(homes, MF_STRING | MF_GRAYED, 0, label.c_str());
        }
        AppendMenuW(homes, MF_SEPARATOR, 0, nullptr);
        if (config::homesFromEnv()) {
            AppendMenuW(homes, MF_STRING | MF_GRAYED, 0, L"Set by CLAUDE_USAGE_HOMES");
        } else {
            AppendMenuW(homes, MF_STRING, ID_ADD_HOME, L"Add home folder…");
            std::vector<std::wstring> stored = config::storedHomes();
            for (size_t i = 0; i < stored.size() && i < 900; i++) {
                AppendMenuW(homes, MF_STRING, ID_REMOVE_HOME + i, (L"Remove " + stored[i]).c_str());
            }
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(homes), L"Claude homes");
        AppendMenuW(menu, MF_STRING, ID_OPEN_STATE, L"Open data folder");
        AppendMenuW(menu, MF_STRING | (startsWithWindows() ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, util::wide(std::string("Claude Usage ") + VERSION_STRING).c_str());
        AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit Claude Usage");

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        command(cmd, rs);
    }

    void command(UINT cmd, const std::vector<reminders::Reminder>& rs) {
        if (cmd == ID_SAMPLE) {
            poll();
        } else if (cmd == ID_QUIT) {
            quit();
        } else if (cmd == ID_STARTUP) {
            setStartsWithWindows(!startsWithWindows());
        } else if (cmd == ID_OPEN_STATE) {
            util::makeDirs(config::stateDir());
            ShellExecuteW(nullptr, L"open", config::stateDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (cmd == ID_ADD_HOME) {
            addHome();
        } else if (cmd >= ID_DONE_REMINDER && cmd - ID_DONE_REMINDER < rs.size()) {
            doneReminder(rs[cmd - ID_DONE_REMINDER].id);
        } else if (cmd >= ID_COPY_REMINDER && cmd - ID_COPY_REMINDER < rs.size()) {
            copyToClipboard(hwnd_, reminders::command(rs[cmd - ID_COPY_REMINDER]));
        } else if (cmd >= ID_REMOVE_HOME && cmd < ID_COPY_REMINDER) {
            std::vector<std::wstring> stored = config::storedHomes();
            size_t i = cmd - ID_REMOVE_HOME;
            if (i < stored.size()) {
                stored.erase(stored.begin() + static_cast<std::ptrdiff_t>(i));
                changeHomes(stored);
            }
        }
    }

    /// A folder picker. The new home goes first, so its .claude\claude-usage becomes the state directory — the point of
    /// adding a container's home is usually that tools inside the container can read status.json.
    void addHome() {
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
            return;
        }
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Choose a home folder that holds .claude (e.g. a container's home, through its bind mount)");
        std::wstring picked;
        if (SUCCEEDED(dlg->Show(hwnd_))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    picked = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
        if (picked.empty()) {
            return;
        }
        if (!util::isDir(util::join(picked, L".claude")) && !util::exists(util::join(picked, L".claude.json")) && !util::isDir(util::join(picked, L".codex"))) {
            std::wstring msg = picked + L"\n\nhas no .claude, .claude.json or .codex in it. Add it anyway?";
            if (MessageBoxW(nullptr, msg.c_str(), APP_NAME, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) {
                return;
            }
        }
        std::vector<std::wstring> stored = config::storedHomes();
        stored.erase(std::remove_if(stored.begin(), stored.end(), [&](const std::wstring& s) { return _wcsicmp(s.c_str(), picked.c_str()) == 0; }), stored.end());
        stored.insert(stored.begin(), picked);
        changeHomes(stored);
    }

    void changeHomes(const std::vector<std::wstring>& stored) {
        std::wstring before = config::stateDir();
        config::setStoredHomes(stored);
        std::wstring after = config::stateDir();
        if (_wcsicmp(before.c_str(), after.c_str()) != 0) {
            for (const wchar_t* f : {L"samples.jsonl", L"accounts.json", L"reminders.json"}) {
                carryOver(before, after, f);
            }
        }
        refresh();
        poll();
    }

    void quit() {
        card_.hide();
        NOTIFYICONDATAW nid = iconData();
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
    }

    // ------------------------------------------------------------ messages
    LRESULT handle(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == taskbarCreated_ && taskbarCreated_) {
            addIcon();   // Explorer restarted
            refresh();
            return 0;
        }
        switch (m) {
        case WM_TRAY:
            switch (LOWORD(l)) {
            case NIN_SELECT:
            case NIN_KEYSELECT:
                if (card_.visible()) {
                    card_.hide();
                } else if (GetTickCount64() - card_.lastHidden() > CLICK_DEBOUNCE_MS) {
                    refresh();
                    card_.show(anchor());
                }
                return 0;
            case WM_CONTEXTMENU:
                contextMenu();
                return 0;
            case NIN_BALLOONUSERCLICK:
                noteGone(true);
                return 0;
            case NIN_BALLOONTIMEOUT:
            case NIN_BALLOONHIDE:
                noteGone(false);
                return 0;
            }
            return 0;
        case WM_TIMER:
            switch (w) {
            case TIMER_REFRESH: refresh(); break;
            case TIMER_TICK: spawnTick(); break;
            case TIMER_POLL: checkPoll(); break;
            case TIMER_RESET:
                KillTimer(hwnd_, TIMER_RESET);
                poll();
                break;
            case TIMER_BALLOON: noteGone(false); break;
            }
            return 0;
        case WM_TICK_DONE:
            if (card_.polling()) {
                checkPoll();
            } else {
                refresh();
            }
            return 0;
        case WM_SHOW_CARD:
            refresh();
            card_.show(anchor());
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_APMRESUMEAUTOMATIC) {
                spawnTick();   // the interval did not run while the machine slept
            }
            return TRUE;
        case WM_SETTINGCHANGE:
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            card_.themeChanged();
            updateIcon(card_.status());
            return 0;
        case WM_DESTROY:
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        args.push_back(util::utf8(argv[i]));
    }
    LocalFree(argv);

    std::vector<std::string> rest = args;
    cli::takeHomeFlags(rest);
    if (cli::isCommand(rest)) {
        // Run from a terminal: print there. Spawned by the tray: stdout is already the log file.
        HANDLE outHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        if ((!outHandle || outHandle == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (con != INVALID_HANDLE_VALUE) {
                SetStdHandle(STD_OUTPUT_HANDLE, con);
                SetStdHandle(STD_ERROR_HANDLE, con);
            }
        }
        return cli::run(args);
    }
    std::vector<std::string> homeArgs;
    for (size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == "--home") {
            homeArgs.push_back(args[i + 1]);
        }
    }

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ClaudeUsageWin.app");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(WINDOW_CLASS, nullptr)) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, WM_SHOW_CARD, 0, 0);
        }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartup(&gdiplus, &gsi, nullptr);
    int code = 0;
    {
        App app(homeArgs);
        if (!app.start(inst)) {
            MessageBoxW(nullptr, L"Could not create the tray window.", APP_NAME, MB_ICONERROR);
            code = 1;
        } else {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            code = static_cast<int>(msg.wParam);
        }
    }
    Gdiplus::GdiplusShutdown(gdiplus);
    CoUninitialize();
    if (single) {
        CloseHandle(single);
    }
    return code;
}

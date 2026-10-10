#pragma once
// The card: the popup the tray icon opens — title and the API-equivalent cost, one tab per subscription (its pair of rings), then per window a label,
// the figures, a bar and the reset; the recommendation; and who it is. Below it, the reminders and Quit.
//
// A click on the tray icon opens it; it closes when it loses focus, on Esc, or on another click on the icon.
// A click on a tab shows that subscription (and the tray icon follows it); a click anywhere else samples now.
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "reminders.h"
#include "ui_model.h"

class Card {
public:
    /// A rectangle in the card's logical (96-dpi) coordinates.
    struct Box {
        float x = 0, y = 0, w = 0, h = 0;
        bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
        Box inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    };

    std::function<void()> onPoll;
    std::function<void()> onSelect;
    std::function<void()> onQuit;
    std::function<void()> onHidden;
    std::function<void(const std::string&)> onCopyReminder;
    std::function<void(const std::string&)> onDoneReminder;

    bool create(HINSTANCE inst);
    void setStatus(ui::Status s);
    void setReminders(std::vector<reminders::Reminder> rs);
    void setPolling(bool polling);
    bool polling() const { return polling_; }
    /// Which account this card — and, through the app, the tray icon — is showing. ONE home: the icon and its
    /// tooltip must never name a different subscription from the one the card is open on.
    const ui::Account* account() const;
    const ui::Status& status() const { return status_; }

    void show(const RECT& anchor);
    void hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    /// GetTickCount64 at the last hide — a tray click that arrives just after the card lost focus to it
    /// is the click that closed it, not a request to open it again.
    ULONGLONG lastHidden() const { return lastHidden_; }
    void themeChanged();
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT handle(UINT m, WPARAM w, LPARAM l);
    void paint();
    void relayout();
    void place();
    float contentHeight() const;
    float cardHeight() const;
    int tabsPerRow() const;
    std::vector<std::pair<int, int>> tabSlots() const;
    int tabRows() const;
    float costH() const;
    std::vector<Box> tabRects() const;
    std::vector<std::pair<Box, Box>> reminderLinks() const;   // (Copy, Done) per reminder row
    Box quitRow() const;
    float adviceHeight(const ui::Account& a) const;
    bool isStale(const ui::Account& a) const;
    int footerLines(const ui::Account& a) const;
    int hitTest(float x, float y) const;

    HWND hwnd_ = nullptr;
    ui::Status status_;
    std::vector<reminders::Reminder> reminders_;
    std::string selectedId_;
    bool polling_ = false;
    RECT anchor_{};
    float scale_ = 1;
    bool dark_ = true;
    int hover_ = -1;
    ULONGLONG lastHidden_ = 0;
    bool placing_ = false;
};

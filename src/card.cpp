#include "card.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#include <cmath>

#include "draw.h"
#include "pace.h"
#include "util.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t CLASS_NAME[] = L"ClaudeUsageWin.Card";

// Geometry, in logical points.
constexpr float WIDTH = 320;
constexpr float PAD = 14;
/// Air between the last window and the recommendation — the two are read separately, so they are spaced apart.
constexpr float ADVICE_GAP = 18;
constexpr float INNER = WIDTH - PAD * 2;
constexpr float TAB_W = 52, TAB_GAP = 13;
// Below the card: the reminders and Quit.
constexpr float SEP_H = 9, REMINDER_ROW_H = 24, QUIT_ROW_H = 26, BOTTOM_PAD = 6;

// Hit-test ids.
constexpr int HIT_NONE = -1, HIT_TAB = 1000, HIT_COPY = 2000, HIT_DONE = 3000, HIT_QUIT = 4000;

struct Fonts {
    Font title{L"Segoe UI", 15, FontStyleBold, UnitPixel};
    Font body{L"Segoe UI", 13, FontStyleRegular, UnitPixel};
    Font semi{L"Segoe UI Semibold", 13, FontStyleRegular, UnitPixel};
    Font advice{L"Segoe UI Semibold", 12.5f, FontStyleRegular, UnitPixel};
    Font caption{L"Segoe UI", 12, FontStyleRegular, UnitPixel};
};

// The GDI+ objects below are created on first use (after GdiplusStartup) and deliberately never destroyed:
// static destructors would run after GdiplusShutdown, and freeing GDI+ objects then crashes the exit.
Fonts& fonts() {
    static Fonts* f = new Fonts;
    return *f;
}

const StringFormat& typographic() {
    static StringFormat* f = [] {
        auto* x = new StringFormat(StringFormat::GenericTypographic());
        x->SetFormatFlags(x->GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces);
        return x;
    }();
    return *f;
}

const StringFormat& ellipsis() {
    static StringFormat* f = [] {
        auto* x = new StringFormat(StringFormat::GenericTypographic());
        x->SetFormatFlags(x->GetFormatFlags() | StringFormatFlagsNoWrap);
        x->SetTrimming(StringTrimmingEllipsisCharacter);
        return x;
    }();
    return *f;
}

Graphics& measurer() {
    static Graphics* g = [] {
        auto* x = new Graphics(new Bitmap(1, 1, PixelFormat32bppARGB));
        x->SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        return x;
    }();
    return *g;
}

SizeF measure(const std::wstring& s, const Font& f, float width = 10000) {
    RectF box;
    measurer().MeasureString(s.c_str(), static_cast<INT>(s.size()), &f, RectF(0, 0, width, 10000), &typographic(), &box);
    return SizeF(box.Width, box.Height);
}

void text(Graphics& g, const std::wstring& s, const Font& f, const Color& c, float x, float y) {
    SolidBrush b(c);
    g.DrawString(s.c_str(), static_cast<INT>(s.size()), &f, PointF(x, y), &typographic(), &b);
}

void roundRect(Graphics& g, const Brush& b, float x, float y, float w, float h, float r) {
    r = std::min({r, w / 2, h / 2});
    if (r <= 0) {
        g.FillRectangle(&b, x, y, w, h);
        return;
    }
    GraphicsPath p;
    p.AddArc(x, y, r * 2, r * 2, 180, 90);
    p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
    p.CloseFigure();
    g.FillPath(&b, &p);
}

std::wstring W(const std::string& s) { return util::wide(s); }

std::string title(const std::string& key) {
    if (key == "session") return "Session (5h)";
    if (key == "weekly_all") return "Week · all models";
    if (util::startsWith(key, "weekly_scoped:")) return "Week · " + key.substr(14);
    if (key == "monthly") return "Month";
    if (util::startsWith(key, "quota:")) return key.substr(6);
    if (util::startsWith(key, "daily")) return "Today" + (key.size() > 6 ? " · " + key.substr(6) : std::string());
    return key;
}

std::string adviceText(const ui::Account& a, bool& onPace) {
    onPace = !a.advice || a.advice->empty();
    if (onPace) {
        return "On pace — nothing to change";
    }
    return util::capitalize(*a.advice);
}

std::string usd(double v) {
    char buf[32];
    // Halves round away from zero.
    if (std::round(v) < 1000) {
        snprintf(buf, sizeof buf, "$%.0f", v);
    } else {
        snprintf(buf, sizeof buf, "$%.1fk", v / 1000);
    }
    return buf;
}

/// A small warning triangle — U+26A0 is not in Segoe UI and GDI+ does no font fallback.
void warningGlyph(Graphics& g, const Color& c, float x, float y, float s) {
    PointF tri[] = {{x + s / 2, y}, {x + s, y + s * 0.9f}, {x, y + s * 0.9f}};
    SolidBrush b(c);
    g.FillPolygon(&b, tri, 3);
    Pen mark(Color(255, 255, 255, 255), std::max(1.0f, s * 0.12f));
    g.DrawLine(&mark, x + s / 2, y + s * 0.32f, x + s / 2, y + s * 0.58f);
    g.DrawLine(&mark, x + s / 2, y + s * 0.68f, x + s / 2, y + s * 0.74f);
}

/// A clock face for the reminder rows — GDI+ has no colour emoji.
void clockGlyph(Graphics& g, const Color& c, float x, float y, float s) {
    Pen pen(c, std::max(1.0f, s * 0.11f));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawEllipse(&pen, x, y, s, s);
    PointF ctr(x + s / 2, y + s / 2);
    g.DrawLine(&pen, ctr.X, ctr.Y, ctr.X, y + s * 0.22f);
    g.DrawLine(&pen, ctr.X, ctr.Y, x + s * 0.72f, ctr.Y);
}

}  // namespace

bool Card::create(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, CLASS_NAME, L"Claude Usage", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        return false;
    }
    // Windows 11 rounds the corners and draws the flyout border; earlier versions ignore both attributes.
    constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE_ = 33, DWMWCP_ROUND_ = 2;
    DWORD corner = DWMWCP_ROUND_;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE_, &corner, sizeof corner);
    themeChanged();
    return true;
}

void Card::themeChanged() {
    dark_ = draw::appsDark();
    constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_ = 20;
    BOOL dark = dark_;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE_, &dark, sizeof dark);
    if (visible()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Card::setStatus(ui::Status s) {
    status_ = std::move(s);
    bool found = false;
    for (const auto& a : status_.accounts) {
        found |= a.id == selectedId_;
    }
    if (!found) {
        selectedId_ = status_.accounts.empty() ? "" : status_.accounts.front().id;
    }
    relayout();
}

void Card::setReminders(std::vector<reminders::Reminder> rs) {
    reminders_ = std::move(rs);
    relayout();
}

void Card::setPolling(bool p) {
    polling_ = p;
    if (visible()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

const ui::Account* Card::account() const {
    for (const auto& a : status_.accounts) {
        if (a.id == selectedId_) {
            return &a;
        }
    }
    return status_.accounts.empty() ? nullptr : &status_.accounts.front();
}

// ---------------------------------------------------------------- layout
int Card::tabsPerRow() const { return std::max(1, static_cast<int>((INNER + TAB_GAP) / (TAB_W + TAB_GAP))); }

/// (row, column) per account: a row fills left to right, and a wrapper's resold models start a row of their own.
std::vector<std::pair<int, int>> Card::tabSlots() const {
    std::vector<std::pair<int, int>> out;
    int row = 0, col = 0;
    for (size_t i = 0; i < status_.accounts.size(); i++) {
        if (i > 0 && (col >= tabsPerRow() || status_.accounts[i].via != status_.accounts[i - 1].via)) {
            row++;
            col = 0;
        }
        out.emplace_back(row, col);
        col++;
    }
    return out;
}

/// The API-equivalent line sits under the title, above the tabs: it counts every Claude profile, so it belongs to no tab.
float Card::costH() const { return status_.hasCost ? 20.0f : 0.0f; }

int Card::tabRows() const { return status_.accounts.size() > 1 ? tabSlots().back().first + 1 : 0; }

std::vector<Card::Box> Card::tabRects() const {
    std::vector<Box> out;
    if (status_.accounts.size() <= 1) {
        return out;
    }
    for (auto [row, col] : tabSlots()) {
        out.push_back({PAD - 4 + static_cast<float>(col) * (TAB_W + TAB_GAP), 40 + costH() + static_cast<float>(row) * 28.0f, TAB_W, 22});
    }
    return out;
}

float Card::adviceHeight(const ui::Account& a) const {
    bool on = false;
    return std::ceil(measure(W(adviceText(a, on)), fonts().advice, INNER).Height);
}

bool Card::isStale(const ui::Account& a) const { return !a.current || util::nowMs() - a.asOf > pace::STALE_AFTER_MS; }

/// Who, and — when the reading is not live — when it was taken.
int Card::footerLines(const ui::Account& a) const { return 1 + (isStale(a) ? 1 : 0); }

/// The tallest account sets the height, so switching tabs never resizes an open card.
float Card::cardHeight() const {
    float fixed = 36 + costH() + static_cast<float>(tabRows()) * 28.0f + (status_.error ? 22.0f : 0.0f) + ADVICE_GAP + 10 + 1 + 9 + 8;
    float h = 100;
    bool any = false;
    for (const auto& a : status_.accounts) {
        float x = fixed + static_cast<float>(a.windows.size()) * 55.0f + adviceHeight(a) + static_cast<float>(footerLines(a)) * 18.0f;
        h = any ? std::max(h, x) : x;
        any = true;
    }
    return h;
}

float Card::contentHeight() const {
    float h = cardHeight();
    if (!reminders_.empty()) {
        h += SEP_H + static_cast<float>(reminders_.size()) * REMINDER_ROW_H;
    }
    return h + SEP_H + QUIT_ROW_H + BOTTOM_PAD;
}

std::vector<std::pair<Card::Box, Card::Box>> Card::reminderLinks() const {
    std::vector<std::pair<Box, Box>> out;
    float y = cardHeight() + SEP_H;
    for (size_t i = 0; i < reminders_.size(); i++) {
        float rowY = y + static_cast<float>(i) * REMINDER_ROW_H;
        out.push_back({{PAD + INNER - 92, rowY + 1, 46, REMINDER_ROW_H - 2}, {PAD + INNER - 44, rowY + 1, 44, REMINDER_ROW_H - 2}});
    }
    return out;
}

Card::Box Card::quitRow() const {
    float y = contentHeight() - BOTTOM_PAD - QUIT_ROW_H;
    return {PAD - 8, y, INNER + 16, QUIT_ROW_H};
}

void Card::relayout() {
    if (visible()) {
        place();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

int Card::hitTest(float x, float y) const {
    auto tabs = tabRects();
    for (size_t i = 0; i < tabs.size(); i++) {
        if (tabs[i].inset(-2, -4).contains(x, y)) {
            return HIT_TAB + static_cast<int>(i);
        }
    }
    auto links = reminderLinks();
    for (size_t i = 0; i < links.size(); i++) {
        if (links[i].first.contains(x, y)) return HIT_COPY + static_cast<int>(i);
        if (links[i].second.contains(x, y)) return HIT_DONE + static_cast<int>(i);
    }
    if (quitRow().contains(x, y)) {
        return HIT_QUIT;
    }
    return HIT_NONE;
}

// ---------------------------------------------------------------- show / hide
void Card::show(const RECT& anchor) {
    anchor_ = anchor;
    place();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Card::hide() {
    if (!visible()) {
        return;
    }
    ShowWindow(hwnd_, SW_HIDE);
    lastHidden_ = GetTickCount64();
    hover_ = HIT_NONE;
    if (onHidden) {
        onHidden();
    }
}

/// Next to the tray icon, on the taskbar's side of the screen, inside the work area of the icon's monitor.
void Card::place() {
    if (placing_) {
        return;
    }
    placing_ = true;
    HMONITOR mon = MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) {
        dx = GetDpiForSystem();
    }
    scale_ = static_cast<float>(dx) / 96.0f;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    RECT work = mi.rcWork, full = mi.rcMonitor;
    int w = static_cast<int>(std::lround(WIDTH * scale_));
    int h = static_cast<int>(std::lround(contentHeight() * scale_));
    int gap = static_cast<int>(std::lround(12 * scale_));
    int cx = (anchor_.left + anchor_.right) / 2, cy = (anchor_.top + anchor_.bottom) / 2;
    int x = cx - w / 2, y = work.bottom - h - gap;
    if (work.top > full.top) {
        y = work.top + gap;
    } else if (work.left > full.left) {
        x = work.left + gap;
        y = cy - h / 2;
    } else if (work.right < full.right) {
        x = work.right - w - gap;
        y = cy - h / 2;
    } else if (cy < (full.top + full.bottom) / 2) {   // an auto-hidden taskbar at the top
        y = anchor_.bottom + gap;
    }
    x = std::clamp(x, static_cast<int>(work.left) + gap, std::max(static_cast<int>(work.left) + gap, static_cast<int>(work.right) - w - gap));
    y = std::clamp(y, static_cast<int>(work.top) + gap, std::max(static_cast<int>(work.top) + gap, static_cast<int>(work.bottom) - h - gap));
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    placing_ = false;
}

// ---------------------------------------------------------------- paint
void Card::paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int pw = rc.right - rc.left, ph = rc.bottom - rc.top;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, pw, ph);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        draw::Palette p = draw::palette(dark_);
        g.Clear(p.background);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.ScaleTransform(scale_, scale_);
        Fonts& f = fonts();
        // `mut` is secondaryLabel's job at an alpha that survives either theme.
        Color mut = p.withLabelAlpha(0.72);
        double now = util::nowMs();
        const ui::Account* acc = account();
        float y = 12;
        if (acc) {
            const ui::Account& a = *acc;
            const char* pn = ui::providerName(a.provider);
            std::string name = a.title ? *a.title : pn ? pn : util::capitalize(a.provider);
            text(g, W(name + (polling_ ? " · updating…" : " usage")), f.title, p.label, PAD + 22, y - 1);
            draw::vendorIcon(g, p, a.vendor, RectF(PAD, y + 1, 16, 16));
            if (status_.hasCost) {
                std::string figs[3] = {usd(status_.costDay), usd(status_.costWeek), usd(status_.costMonth)};
                std::string full = "API-equiv day " + figs[0] + " · week " + figs[1] + " · month " + figs[2];
                // Short labels once the full ones would not fit the card.
                std::string line = measure(W(full), f.body).Width > INNER ? "API-equiv d " + figs[0] + " · w " + figs[1] + " · m " + figs[2] : full;
                text(g, W(line), f.body, mut, PAD, y + 22);
            }
            auto slots = tabSlots();
            auto rects = tabRects();
            for (size_t i = 0; i < rects.size(); i++) {
                const ui::Account& t = status_.accounts[i];
                const Box& r = rects[i];
                if (t.id == a.id) {
                    SolidBrush sel(p.withLabelAlpha(0.14));
                    roundRect(g, sel, r.x, r.y, r.w, r.h, 7);
                } else if (hover_ == HIT_TAB + static_cast<int>(i)) {
                    SolidBrush hov(p.withLabelAlpha(0.07));
                    roundRect(g, hov, r.x, r.y, r.w, r.h, 7);
                }
                draw::vendorIcon(g, p, t.vendor, RectF(r.x + 5, r.y + r.h / 2 - 5.5f, 11, 11));
                const ui::Win *w1 = nullptr, *w2 = nullptr;
                draw::ringPair(t.windows, w1, w2);
                draw::ring(g, p, w1, t.current, false, PointF(r.x + 27, r.y + r.h / 2), 4.5f, 2.4f);
                draw::ring(g, p, w2, t.current, false, PointF(r.x + 41, r.y + r.h / 2), 4.5f, 2.4f);
                if (i + 1 < slots.size() && slots[i + 1].first == slots[i].first) {
                    SolidBrush sep(p.separator);
                    g.FillRectangle(&sep, r.x + r.w + TAB_GAP / 2, r.y + 4, 1.0f, r.h - 8);
                }
            }
            y += 24 + costH() + static_cast<float>(tabRows()) * 28;
            if (status_.error) {
                warningGlyph(g, p.orange, PAD, y + 2, 12);
                text(g, W(*status_.error), f.caption, p.orange, PAD + 17, y);
                y += 22;
            }
            for (const ui::Win& w : a.windows) {
                y += 8;
                text(g, W(title(w.key)), f.semi, p.label, PAD, y);
                std::wstring pct = W(util::jsNum(std::round(w.percent)) + "%");
                std::wstring speedLabel = L"  speed ";
                std::wstring speed = W(util::jsNum(std::round(w.projected)));
                float wp = measure(pct, f.semi).Width;
                float total = wp + (a.current ? measure(speedLabel, f.body).Width + measure(speed, f.semi).Width : 0);
                float x = PAD + INNER - total;
                text(g, pct, f.semi, p.label, x, y);
                if (a.current) {
                    x += wp;
                    text(g, speedLabel, f.body, mut, x, y);
                    x += measure(speedLabel, f.body).Width;
                    text(g, speed, f.semi, draw::levelColor(p, w.projected), x, y);
                }
                y += 21;
                SolidBrush track(p.withLabelAlpha(0.12));
                roundRect(g, track, PAD, y, INNER, 6, 3);
                double pc = std::clamp(w.percent, 0.0, 100.0);
                if (pc > 0) {
                    SolidBrush fill(draw::usedColor(p, pc));
                    roundRect(g, fill, PAD, y, std::max(6.0f, static_cast<float>(INNER * pc / 100)), 6, 3);
                }
                y += 9;
                // Hours left were true at the tick; a remembered account can be read long after, so count from now.
                std::string reset = w.hoursLeft ? "resets in " + ui::countdown(std::max(0.0, *w.hoursLeft - (now - status_.accountsT) / 3600000))
                                                : a.current ? "idle" : "reset — ready";
                text(g, W(reset), f.body, !w.hoursLeft && !a.current ? p.green : mut, PAD, y);
                y += 17;
            }
            y += ADVICE_GAP;
            bool on = false;
            std::string adv = adviceText(a, on);
            float ah = adviceHeight(a);
            SolidBrush advBrush(on || util::startsWith(adv, "Ready") ? p.green : p.label);
            std::wstring wadv = W(adv);
            g.DrawString(wadv.c_str(), static_cast<INT>(wadv.size()), &f.advice, RectF(PAD, y, INNER, ah + 2), &typographic(), &advBrush);

            float cardH = cardHeight();
            float sepY = cardH - 8 - static_cast<float>(footerLines(a)) * 18.0f - 10;
            // The quietest lines on the card, so a scrim gives them a floor and separates the footer on its own.
            SolidBrush scrim(p.scrim);
            roundRect(g, scrim, PAD - 6, sepY + 4, INNER + 12, cardH - sepY - 8, 7);
            y = sepY + 10;
            // The dot says whether this is a live login, so no line is spent on saying it.
            SolidBrush dot(a.current ? p.green : p.withLabelAlpha(0.3));
            g.FillEllipse(&dot, PAD, y + 5, 7.0f, 7.0f);
            std::string who = a.email ? *a.email : "earlier account";
            if (a.plan) who += " · " + *a.plan;
            if (a.via) who += " · via " + *a.via;
            SolidBrush mutBrush(mut);
            std::wstring wwho = W(who);
            g.DrawString(wwho.c_str(), static_cast<INT>(wwho.size()), &f.body, RectF(PAD + 13, y, INNER - 13, 18), &ellipsis(), &mutBrush);
            y += 18;
            if (isStale(a)) {
                text(g, W("last checked: " + util::localFormat(a.asOf, "%b %#d, %H:%M")), f.body, mut, PAD + 13, y);
            }
        }

        // A separator, the reminders, a separator, Quit.
        float y2 = cardHeight();
        SolidBrush sep(p.separator);
        if (!reminders_.empty()) {
            g.FillRectangle(&sep, PAD - 4, y2 + SEP_H / 2, INNER + 8, 1.0f);
            y2 += SEP_H;
            auto links = reminderLinks();
            for (size_t i = 0; i < reminders_.size(); i++) {
                const auto& r = reminders_[i];
                float rowY = y2 + static_cast<float>(i) * REMINDER_ROW_H;
                clockGlyph(g, mut, PAD, rowY + 6, 11);
                std::string when = r.at <= now ? "due" : util::localFormat(r.at, "%a %b %#d, %H:%M");
                std::wstring label = W(r.title + " — " + when);
                SolidBrush lb(r.at <= now ? p.label : mut);
                g.DrawString(label.c_str(), static_cast<INT>(label.size()), &f.body, RectF(PAD + 17, rowY + 4, INNER - 17 - 96, 18), &ellipsis(), &lb);
                const Box boxes[2] = {links[i].first, links[i].second};
                const wchar_t* names[2] = {L"Copy", L"Done"};
                for (int k = 0; k < 2; k++) {
                    int id = (k == 0 ? HIT_COPY : HIT_DONE) + static_cast<int>(i);
                    if (hover_ == id) {
                        SolidBrush hb(p.withLabelAlpha(0.09));
                        roundRect(g, hb, boxes[k].x, boxes[k].y, boxes[k].w, boxes[k].h, 5);
                    }
                    float tw = measure(names[k], f.body).Width;
                    text(g, names[k], f.body, p.blue, boxes[k].x + (boxes[k].w - tw) / 2, boxes[k].y + 3);
                }
            }
            y2 += static_cast<float>(reminders_.size()) * REMINDER_ROW_H;
        }
        g.FillRectangle(&sep, PAD - 4, y2 + SEP_H / 2, INNER + 8, 1.0f);
        Box q = quitRow();
        if (hover_ == HIT_QUIT) {
            SolidBrush hb(p.withLabelAlpha(0.09));
            roundRect(g, hb, q.x, q.y, q.w, q.h, 5);
        }
        text(g, L"Quit Claude Usage", f.body, p.label, PAD, q.y + 5);
        std::wstring hint = L"Ctrl+Q";
        text(g, hint, f.body, p.withLabelAlpha(0.45), PAD + INNER - measure(hint, f.body).Width, q.y + 5);
    }
    BitBlt(dc, 0, 0, pw, ph, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

// ---------------------------------------------------------------- input
LRESULT CALLBACK Card::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        reinterpret_cast<Card*>(cs->lpCreateParams)->hwnd_ = h;
    }
    auto* self = reinterpret_cast<Card*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT Card::handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            hide();
        }
        return 0;
    case WM_MOUSEMOVE: {
        int hit = hitTest(static_cast<float>(GET_X_LPARAM(l)) / scale_, static_cast<float>(GET_Y_LPARAM(l)) / scale_);
        if (hit != hover_) {
            hover_ = hit;
            InvalidateRect(hwnd_, nullptr, FALSE);
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&t);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hover_ != HIT_NONE) {
            hover_ = HIT_NONE;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        int hit = hitTest(static_cast<float>(pt.x) / scale_, static_cast<float>(pt.y) / scale_);
        SetCursor(LoadCursorW(nullptr, hit >= HIT_COPY && hit < HIT_QUIT ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_LBUTTONUP: {
        int hit = hitTest(static_cast<float>(GET_X_LPARAM(l)) / scale_, static_cast<float>(GET_Y_LPARAM(l)) / scale_);
        if (hit >= HIT_TAB && hit < HIT_COPY) {
            selectedId_ = status_.accounts[static_cast<size_t>(hit - HIT_TAB)].id;
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (onSelect) onSelect();
        } else if (hit >= HIT_COPY && hit < HIT_DONE) {
            if (onCopyReminder) onCopyReminder(reminders_[static_cast<size_t>(hit - HIT_COPY)].id);
        } else if (hit >= HIT_DONE && hit < HIT_QUIT) {
            if (onDoneReminder) onDoneReminder(reminders_[static_cast<size_t>(hit - HIT_DONE)].id);
        } else if (hit == HIT_QUIT) {
            if (onQuit) onQuit();
        } else if (onPoll) {
            onPoll();
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
            hide();
        } else if (w == 'Q' && (GetKeyState(VK_CONTROL) & 0x8000) && onQuit) {
            onQuit();
        }
        return 0;
    case WM_DPICHANGED:
        if (!placing_) {
            place();
        } else {
            scale_ = static_cast<float>(LOWORD(w)) / 96.0f;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE:
        themeChanged();
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

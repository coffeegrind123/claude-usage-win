#include "draw.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "pace.h"
#include "util.h"

using namespace Gdiplus;

namespace draw {

namespace {

constexpr float PI = 3.14159265358979f;

bool personalizeLight(const wchar_t* value) {
    DWORD v = 0, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", value, RRF_RT_REG_DWORD, nullptr, &v, &size) !=
        ERROR_SUCCESS) {
        return false;   // no value: the dark default of Windows 11
    }
    return v != 0;
}

Color rgb(int r, int g, int b, int a = 255) { return Color(static_cast<BYTE>(a), static_cast<BYTE>(r), static_cast<BYTE>(g), static_cast<BYTE>(b)); }

/// Gemini's star: four points, the curves pulled a quarter of the way in — fat enough to read at 11 px.
void starPath(GraphicsPath& path, PointF c, float u) {
    PointF pts[4] = {{c.X, c.Y - u}, {c.X + u, c.Y}, {c.X, c.Y + u}, {c.X - u, c.Y}};
    PointF prev = pts[3];
    for (const PointF& p : pts) {
        PointF c1(c.X + (prev.X - c.X) * 0.25f, c.Y + (prev.Y - c.Y) * 0.25f);
        PointF c2(c.X + (p.X - c.X) * 0.25f, c.Y + (p.Y - c.Y) * 0.25f);
        path.AddBezier(prev, c1, c2, p);
        prev = p;
    }
    path.CloseFigure();
}

}  // namespace

Color Palette::alpha(const Color& c, double a) const {
    return Color(static_cast<BYTE>(std::clamp(a, 0.0, 1.0) * 255), c.GetR(), c.GetG(), c.GetB());
}

Palette palette(bool dark) {
    Palette p;
    p.dark = dark;
    if (dark) {
        p.background = rgb(0x2B, 0x2B, 0x2B);
        p.label = rgb(255, 255, 255, 235);
        p.green = rgb(0x30, 0xD1, 0x58);
        p.orange = rgb(0xFF, 0x9F, 0x0A);
        p.red = rgb(0xFF, 0x45, 0x3A);
        p.blue = rgb(0x0A, 0x84, 0xFF);
        p.separator = rgb(255, 255, 255, 40);
        p.scrim = rgb(0, 0, 0, 102);
    } else {
        p.background = rgb(0xF9, 0xF9, 0xF9);
        p.label = rgb(0, 0, 0, 222);
        p.green = rgb(0x28, 0xA7, 0x45);   // systemGreen darkened a step: #34C759 is too light on a light card
        p.orange = rgb(0xE8, 0x87, 0x00);
        p.red = rgb(0xFF, 0x3B, 0x30);
        p.blue = rgb(0x00, 0x67, 0xC0);
        p.separator = rgb(0, 0, 0, 30);
        p.scrim = rgb(255, 255, 255, 140);
    }
    return p;
}

bool appsDark() { return !personalizeLight(L"AppsUseLightTheme"); }
bool taskbarDark() { return !personalizeLight(L"SystemUsesLightTheme"); }

Color levelColor(const Palette& p, double projected) {
    return projected > 100 ? p.red : projected > pace::TARGET ? p.orange : p.green;
}

Color usedColor(const Palette& p, double percent) {
    return percent >= 90 ? p.red : percent >= 70 ? p.orange : p.green;
}

void ring(Graphics& g, const Palette& p, const ui::Win* w, bool live, bool stale, PointF c, float r, float line) {
    double value = live ? (w ? w->projected : 0) : (w ? w->percent : 0);
    if (w && !stale && (live ? value > 100 : value >= 100)) {
        SolidBrush red(p.red);
        float R = r + line / 2;
        g.FillEllipse(&red, c.X - R, c.Y - R, R * 2, R * 2);
        return;
    }
    bool fresh = !live && w && value == 0;
    Pen track(fresh ? p.alpha(p.green, 0.85) : p.withLabelAlpha(0.25), line);
    g.DrawEllipse(&track, c.X - r, c.Y - r, r * 2, r * 2);
    if (!w || value <= 0) {
        return;
    }
    Color arcColor = stale ? p.withLabelAlpha(0.5) : live ? levelColor(p, value) : usedColor(p, value);
    Pen arc(arcColor, line);
    arc.SetStartCap(LineCapRound);
    arc.SetEndCap(LineCapRound);
    float sweep = static_cast<float>(360 * std::min(value, 100.0) / 100);
    if (sweep >= 360) {
        g.DrawEllipse(&arc, c.X - r, c.Y - r, r * 2, r * 2);
    } else {
        g.DrawArc(&arc, c.X - r, c.Y - r, r * 2, r * 2, -90, sweep);
    }
}

void ringPair(const std::vector<ui::Win>& ws, const ui::Win*& first, const ui::Win*& second) {
    first = second = nullptr;
    for (const auto& w : ws) {
        if (w.key == "session" && !first) first = &w;
        if (w.key == "weekly_all" && !second) second = &w;
    }
    if (first || second) {
        return;
    }
    first = ws.empty() ? nullptr : &ws[0];
    second = ws.size() > 1 ? &ws[1] : nullptr;
}

void vendorIcon(Graphics& g, const Palette& p, const std::string& vendor, RectF r) {
    PointF c(r.X + r.Width / 2, r.Y + r.Height / 2);
    float u = std::min(r.Width, r.Height) / 2;
    if (vendor == "claude") {
        Pen rays(rgb(0xD9, 0x78, 0x57), std::max(1.3f, u * 0.28f));
        rays.SetStartCap(LineCapRound);
        rays.SetEndCap(LineCapRound);
        for (int k = 0; k < 6; k++) {
            float t = static_cast<float>(k) * PI / 6, dx = std::cos(t) * u * 0.9f, dy = std::sin(t) * u * 0.9f;
            g.DrawLine(&rays, c.X - dx, c.Y - dy, c.X + dx, c.Y + dy);
        }
    } else if (vendor == "openai") {
        Pen petal(p.label, std::max(1.0f, u * 0.16f));
        float q = u * 0.46f;
        for (int k = 0; k < 6; k++) {
            float t = static_cast<float>(k) * PI / 3 + PI / 6;
            g.DrawEllipse(&petal, c.X + std::cos(t) * q - q, c.Y + std::sin(t) * q - q, q * 2, q * 2);
        }
    } else if (vendor == "gemini") {
        GraphicsPath star;
        starPath(star, c, u);
        SolidBrush blue(rgb(0x4F, 0x8C, 0xF7));
        g.FillPath(&blue, &star);
    } else if (vendor == "cursor") {
        // An arrow pointer, tip at the top left.
        PointF pts[] = {{c.X - u * 0.55f, c.Y - u * 0.85f}, {c.X + u * 0.6f, c.Y + u * 0.15f}, {c.X + u * 0.02f, c.Y + u * 0.22f},
                        {c.X + u * 0.3f, c.Y + u * 0.85f}, {c.X + u * 0.05f, c.Y + u * 0.95f}, {c.X - u * 0.22f, c.Y + u * 0.33f},
                        {c.X - u * 0.55f, c.Y + u * 0.62f}};
        SolidBrush b(p.label);
        g.FillPolygon(&b, pts, 7);
    } else if (vendor == "copilot") {
        // An aeroplane seen from above, nose up.
        PointF pts[] = {{c.X, c.Y - u},          {c.X + u * 0.12f, c.Y - u * 0.75f}, {c.X + u * 0.12f, c.Y - u * 0.2f}, {c.X + u, c.Y + u * 0.25f},
                        {c.X + u, c.Y + u * 0.42f}, {c.X + u * 0.12f, c.Y + u * 0.18f}, {c.X + u * 0.1f, c.Y + u * 0.68f}, {c.X + u * 0.4f, c.Y + u * 0.9f},
                        {c.X + u * 0.4f, c.Y + u},  {c.X, c.Y + u * 0.9f},           {c.X - u * 0.4f, c.Y + u},         {c.X - u * 0.4f, c.Y + u * 0.9f},
                        {c.X - u * 0.1f, c.Y + u * 0.68f}, {c.X - u * 0.12f, c.Y + u * 0.18f}, {c.X - u, c.Y + u * 0.42f}, {c.X - u, c.Y + u * 0.25f},
                        {c.X - u * 0.12f, c.Y - u * 0.2f}, {c.X - u * 0.12f, c.Y - u * 0.75f}};
        SolidBrush b(p.label);
        g.FillPolygon(&b, pts, static_cast<INT>(std::size(pts)));
    } else {
        // Sparkles: a large star and a small one.
        SolidBrush b(p.label);
        GraphicsPath big, little;
        starPath(big, PointF(c.X - u * 0.2f, c.Y + u * 0.15f), u * 0.8f);
        starPath(little, PointF(c.X + u * 0.6f, c.Y - u * 0.55f), u * 0.4f);
        g.FillPath(&b, &big);
        g.FillPath(&b, &little);
    }
}

HICON trayIcon(const ui::Account* a, const ui::Status& s, bool live, int size) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.Clear(Color(0, 0, 0, 0));
        Palette p = palette(taskbarDark());
        const ui::Win *w1 = nullptr, *w2 = nullptr;
        ringPair(a ? a->windows : s.windows, w1, w2);
        bool stale = live && util::nowMs() - (a ? a->asOf : s.t) > pace::STALE_AFTER_MS;
        // Two rings side by side in the square: each takes half the width, the outer edge of the stroke
        // touching the icon's edge, so they read at 16 px and scale with the taskbar.
        float S = static_cast<float>(size);
        float line = std::max(1.6f, S * 0.12f);
        float r = S / 4 - line / 2 - S * 0.01f;
        ring(g, p, w1, live, stale, PointF(S * 0.25f, S / 2), r, line);
        ring(g, p, w2, live, stale, PointF(S * 0.75f, S / 2), r, line);
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

}  // namespace draw

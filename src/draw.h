#pragma once
// GDI+ drawing shared by the tray icon and the card: the colours, the speed ring, the vendor marks.
#include <windows.h>
#include <objidl.h>

#include <algorithm>
// The GDI+ headers call min/max unqualified, which NOMINMAX takes away.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#pragma warning(push, 3)
#pragma warning(disable : 4458)
#include <gdiplus.h>
#pragma warning(pop)

#include <string>
#include <vector>

#include "ui_model.h"

namespace draw {

/// The card's colours, for a light or a dark surface.
struct Palette {
    bool dark = true;
    Gdiplus::Color background, label, green, orange, red, blue, separator, scrim;
    Gdiplus::Color alpha(const Gdiplus::Color& c, double a) const;
    Gdiplus::Color withLabelAlpha(double a) const { return alpha(label, a); }
};
Palette palette(bool dark);
/// The app theme (card) and the system theme (taskbar), from the Personalize settings.
bool appsDark();
bool taskbarDark();

/// Speed ladder: past 100 red, past 80 amber.
Gdiplus::Color levelColor(const Palette& p, double projected);
/// Used ladder: from 90 red, from 70 amber.
Gdiplus::Color usedColor(const Palette& p, double percent);

/// One ring. A live login shows SPEED (projected % at reset; past 100 = solid red disc). A remembered one shows
/// what is USED, since nothing is being spent there — and a window that has reset since draws as a full green
/// outline, which is the signal to switch back.
void ring(Gdiplus::Graphics& g, const Palette& p, const ui::Win* w, bool live, bool stale, Gdiplus::PointF c, float r, float line);
/// Session then week where a provider has them; otherwise its own first two windows.
void ringPair(const std::vector<ui::Win>& ws, const ui::Win*& first, const ui::Win*& second);
/// Whose MODEL is being spent, as a mark: Claude's burst, OpenAI's blossom, Gemini's four-point star, and
/// simple glyphs for the rest. Drawn here; no logo files ship.
void vendorIcon(Gdiplus::Graphics& g, const Palette& p, const std::string& vendor, Gdiplus::RectF r);

/// The tray icon: the two rings of the account whose tab is open, at the taskbar's icon size.
HICON trayIcon(const ui::Account* a, const ui::Status& s, bool live, int size);

}  // namespace draw

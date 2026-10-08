#include "hyprdrop.hpp"

static SHyprdropLayout hyprdropAnimateLayout(SHyprdropLayout L, const CBox& U, const Vector2D& S);

// Marking of A's tile: smaller than the others, with a bar under it.
// A's slot is narrower, so the other tiles get more room.
static constexpr double     HYPRDROP_ACTIVE_SCALE   = 0.65;

static constexpr double     HYPRDROP_ACTIVE_BAR_GAP = 8.0; // logical px between A's tile and its bar

// Room under each row of tiles for the tiles' names (logical px).
static constexpr double HYPRDROP_LABEL_SPACE = 24.0;

// ---------------------------------------------------------------- animations

// Open / close: the top view grows from the usable area (windows at their real place)
// to its box and the strip comes up from below; closing plays it backwards toward the
// workspace the monitor lands on. While closing, the overview is drawn but input goes
// to the desktop again.
static constexpr double HYPRDROP_OPEN_MS  = 250.0;

static std::chrono::steady_clock::time_point g_hyprdropAnimStart;

double hyprdropEase(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return 1.0 - std::pow(1.0 - t, 3.0); // ease out cubic
}

double hyprdropElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// 0 = closed (real layout), 1 = open.
double hyprdropOpenProgress() {
    const double TO = g_hyprdropOpen ? 1.0 : 0.0;
    return g_hyprdropAnimFrom + (TO - g_hyprdropAnimFrom) * hyprdropEase(hyprdropElapsedMs(g_hyprdropAnimStart) / HYPRDROP_OPEN_MS);
}

// Called right after g_hyprdropOpen changed.
void hyprdropStartOpenAnim() {
    const bool WASOPEN = !g_hyprdropOpen;
    // Progress at this instant, computed with the previous direction.
    g_hyprdropOpen     = WASOPEN;
    const double NOW   = (WASOPEN || g_hyprdropClosing) ? hyprdropOpenProgress() : 0.0;
    g_hyprdropOpen     = !WASOPEN;
    g_hyprdropAnimFrom = NOW;
    g_hyprdropAnimStart = std::chrono::steady_clock::now();
    g_hyprdropClosing  = !g_hyprdropOpen;
}

// Drawn at all: open, or still closing.
bool hyprdropVisible() {
    return g_hyprdropOpen || g_hyprdropClosing;
}

SHyprdropFit hyprdropFitOf(WORKSPACEID id) {
    const auto IT = g_hyprdropFits.find(id);
    return IT == g_hyprdropFits.end() ? SHyprdropFit{} : IT->second;
}

// View point -> usable-area point of workspace `id` (inverse of the mapping below, gap aside).
Vector2D hyprdropViewToUsable(WORKSPACEID id, const CBox& view, const Vector2D& P) {
    const double k   = view.w / g_hyprdropCaptureUsable.w;
    const auto   FIT = hyprdropFitOf(id);
    return ((P - view.pos()) / k - FIT.offset) / FIT.scale;
}

// The view shows the usable area: `k` = view size / usable size.
SHyprdropPlaced hyprdropPlace(const SHyprdropWinCapture& c, const CBox& view) {
    const double k   = view.w / g_hyprdropCaptureUsable.w;
    const auto   FIT = hyprdropFitOf(c.ws);
    const auto   FP  = [&](const Vector2D& pt) { return pt * FIT.scale + FIT.offset; };

    const CBox     B = {FP(c.box.pos()), c.box.size() * FIT.scale}; // window box after the fit
    const Vector2D C = B.pos() + B.size() / 2.0;

    // Each window shrinks by the missing gap on each axis (the content is squeezed very
    // slightly, by gap / size).
    const double   TARGET = (2 * HYPRDROP_GAPS_IN_REF + HYPRDROP_WINDOW_GAP) / g_hyprdropCaptureScale;
    const Vector2D GAP    = (FIT.gap.x >= 0 ? FIT.gap : Vector2D{TARGET, TARGET}) * hyprdropOpenProgress();
    Vector2D       s      = {1.0, 1.0};
    if (B.w > 2 * GAP.x && B.h > 2 * GAP.y)
        s = {(B.w - GAP.x) / B.w, (B.h - GAP.y) / B.h};

    // usable-area point -> view point
    const auto MAP = [&](const Vector2D& pt) { return view.pos() + (C + (FP(pt) - C) * s) * k; };

    // The texture covers the whole monitor it was rendered on, whose corner is at
    // -usablePos here.
    const Vector2D TEXPOS = -c.usablePos;
    return {CBox{MAP(TEXPOS), c.monSize * FIT.scale * s * k}, CBox{MAP(c.box.pos()), B.size() * s * k}};
}

// Topmost captured window of workspace `id` under `P` when that workspace is drawn in `view`.
std::optional<std::pair<PHLWINDOW, SHyprdropPlaced>> hyprdropCapturedWindowAt(WORKSPACEID id, const CBox& view, const Vector2D& P) {
    const auto CAPS = hyprdropCaptureOf(id);
    if (!CAPS)
        return std::nullopt;
    for (auto it = CAPS->rbegin(); it != CAPS->rend(); ++it) {
        const auto PLACED = hyprdropPlace(*it, view);
        if (!PLACED.win.containsPoint(P))
            continue;
        const auto W = it->win.lock();
        if (!W) {
            dbg("hit: window under the cursor was closed since the capture");
            return std::nullopt;
        }
        return std::make_pair(W, PLACED);
    }
    return std::nullopt;
}

// Open / close animation applied to a final layout: the top view between the usable area
// U and its box, the strip slid down off the screen.
static SHyprdropLayout hyprdropAnimateLayout(SHyprdropLayout L, const CBox& U, const Vector2D& S) {
    const double P = hyprdropOpenProgress();
    if (P >= 1.0)
        return L;
    L.top = {U.x + (L.top.x - U.x) * P, U.y + (L.top.y - U.y) * P, U.w + (L.top.w - U.w) * P, U.h + (L.top.h - U.h) * P};
    const double DY = (1.0 - P) * (S.y - L.strip.y);
    L.strip.y += DY;
    for (auto& t : L.tiles)
        t.y += DY;
    for (auto& t : L.labels)
        t.y += DY;
    if (!L.activeBar.empty())
        L.activeBar.y += DY;
    if (!L.trash.empty())
        L.trash.y += DY;
    return L;
}

SHyprdropLayout hyprdropLayout(const Vector2D& S) {
    SHyprdropLayout L;
    const bool      MAGICROW = hyprdropHasMagicTile();
    const size_t    n        = g_hyprdropTiles.size() - (MAGICROW ? 1 : 0); // tiles of the numbered row
    const int       rows     = MAGICROW ? 2 : 1;

    // Everything is laid out inside the usable area, so the bar never covers the strip
    // (whole monitor until the first capture). Views also take its aspect ratio.
    const bool   HASU   = g_hyprdropCaptureUsable.w > 0 && g_hyprdropCaptureUsable.h > 0;
    const CBox   U      = HASU ? g_hyprdropCaptureUsable : CBox{0, 0, S.x, S.y};
    const double aspect = U.w / U.h;

    // The strip grows when it has the second row; the top view takes what's left.
    const double stripH = U.h * (MAGICROW ? 0.25 : 0.15);
    L.strip             = {U.x, U.y + U.h - stripH, U.w, stripH};

    const double margin = U.h * 0.02;
    const double availH = L.strip.y - U.y - 2 * margin;
    double       TH     = availH;
    double       TW     = TH * aspect;
    if (TW > U.w * 0.96) { // very wide usable area: limited by the width
        TW = U.w * 0.96;
        TH = TW / aspect;
    }
    L.top = {U.x + (U.w - TW) / 2, U.y + margin + (availH - TH) / 2, TW, TH};

    if (g_hyprdropTiles.empty())
        return hyprdropAnimateLayout(L, U, S);

    const double gap    = U.w * 0.01;
    const double rowGap = U.h * 0.015;
    const double availW = U.w * 0.96;
    // A's slot counts for HYPRDROP_ACTIVE_SCALE of a tile, so the others get more room.
    const int    A     = hyprdropActiveTile();
    const bool   AROW  = A >= 0 && A < (int)n;
    const double UNITS = n - (AROW ? 1.0 - HYPRDROP_ACTIVE_SCALE : 0.0);
    double       h     = (stripH - (rows + 1) * rowGap - rows * HYPRDROP_LABEL_SPACE) / rows;
    double       w     = h * aspect;

    if (n > 0 && UNITS * w + (n - 1) * gap > availW) { // too many tiles: shrink them
        w = (availW - (n - 1) * gap) / UNITS;
        h = w / aspect;
    }

    // Rows (tiles + their names) centered vertically in the strip.
    const double ROWH = h + HYPRDROP_LABEL_SPACE;
    const double y0   = L.strip.y + (stripH - (rows * ROWH + (rows - 1) * rowGap)) / 2;

    double x = U.x + (U.w - (UNITS * w + (n - 1) * gap)) / 2;
    for (size_t i = 0; i < n; ++i) {
        const double SC = AROW && (int)i == A ? HYPRDROP_ACTIVE_SCALE : 1.0;
        L.tiles.push_back(CBox{x, y0 + (h - h * SC) / 2, w * SC, h * SC});
        x += w * SC + gap;
    }
    const double BARH = std::max(2.0, h * HYPRDROP_ACTIVE_SCALE * 0.05);
    if (AROW) // A's tile and its bar are centered together in the row
        L.tiles[A].y = y0 + (h - (L.tiles[A].h + HYPRDROP_ACTIVE_BAR_GAP + BARH)) / 2;

    // special:magic on the second row, with the trash (a smaller square, vertically
    // centered) on its right, both centered.
    if (MAGICROW) {
        const double T   = h * 0.55;
        const double TGAP = gap * 3; // the trash stands a bit apart from the tile
        const double X2  = U.x + (U.w - (w + TGAP + T)) / 2;
        L.tiles.push_back(CBox{X2, y0 + ROWH + rowGap, w, h});
        L.trash = CBox{X2 + w + TGAP, y0 + ROWH + rowGap + (h - T) / 2, T, T};
    }

    // Names: under each row, centered under their tile, all on the row's baseline.
    for (size_t i = 0; i < L.tiles.size(); ++i) {
        const double ROWBOTTOM = i < n ? y0 + h : y0 + ROWH + rowGap + h;
        L.labels.push_back(CBox{L.tiles[i].x, ROWBOTTOM, L.tiles[i].w, HYPRDROP_LABEL_SPACE});
    }

    // The bar sits in the space freed under A's smaller tile.
    if (A >= 0) {
        const CBox& t = L.tiles[A];
        L.activeBar   = CBox{t.x, t.y + t.h + HYPRDROP_ACTIVE_BAR_GAP, t.w, BARH};
    }

    return hyprdropAnimateLayout(L, U, S);
}

int hyprdropTileAt(const SHyprdropLayout& L, const Vector2D& P) {
    for (size_t i = 0; i < L.tiles.size(); ++i) {
        if (L.tiles[i].containsPoint(P))
            return (int)i;
    }
    return -1;
}

// Workspace shown in the top view: D (hovering a tile makes it D).
WORKSPACEID hyprdropShownID() {
    return g_hyprdropTopID;
}

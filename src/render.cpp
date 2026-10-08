#include "hyprdrop.hpp"

static void hyprdropAddRect(const CBox& box, const CHyprColor& color, double round = 0);
static void hyprdropAddBorder(const CBox& box, double width, const CHyprColor& color);
static CHyprColor hyprdropActiveBorderColor();
static void hyprdropAddRoundBorder(const CBox& box, double round, float roundingPower, double width, std::optional<CHyprColor> color = std::nullopt);
static std::string hyprdropDesktopIconName(const std::string& cls);
static SP<Render::ITexture> hyprdropIconOf(PHLWINDOW w, double monScale);
static void hyprdropAddIcon(PHLWINDOW w, const CBox& win, double monScale, const CBox& clip = {}, float alpha = 1.F);
static void hyprdropAddTileLabel(WORKSPACEID id, const CBox& tile, double monScale);
static void hyprdropAddWorkspace(WORKSPACEID id, const CBox& box, double monScale, const CHyprColor& bg, std::optional<CBox> clip = std::nullopt);

// Used when general:col.active_border can't be read.
static const CHyprColor HYPRDROP_ACTIVE_BAR_COLOR = CHyprColor{1.0, 0.6, 0.1, 1.0};

// Duration of the workspace slide in the top view.
static constexpr double HYPRDROP_SLIDE_MS = 200.0;

static void hyprdropAddRect(const CBox& box, const CHyprColor& color, double round) {
    CRectPassElement::SRectData rect;
    rect.box   = box;
    rect.color = color;
    rect.round = std::round(round);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(rect));
}

// Outline of `box`, `width` thick, outside it (four thin rectangles, nothing inside).
static void hyprdropAddBorder(const CBox& box, double width, const CHyprColor& color) {
    hyprdropAddRect({box.x - width, box.y - width, box.w + 2 * width, width}, color);
    hyprdropAddRect({box.x - width, box.y + box.h, box.w + 2 * width, width}, color);
    hyprdropAddRect({box.x - width, box.y, width, box.h}, color);
    hyprdropAddRect({box.x + box.w, box.y, width, box.h}, color);
}

// Ghost look: transparent enough to see the tile under it, smaller than the window.
static constexpr double HYPRDROP_GHOST_ALPHA = 0.5;

// First color of general:col.active_border (the bar under A's tile and the ghost frame use
// it). Falls back to HYPRDROP_ACTIVE_BAR_COLOR if it can't be read.
static CHyprColor hyprdropActiveBorderColor() {
    static CConfigValue<Config::IComplexConfigValue> ACTIVEBORDER("general:col.active_border");
    if (!ACTIVEBORDER.good())
        return HYPRDROP_ACTIVE_BAR_COLOR;
    const auto* GRAD = dynamic_cast<Config::CGradientValueData*>(ACTIVEBORDER.ptr());
    if (!GRAD || GRAD->m_colors.empty())
        return HYPRDROP_ACTIVE_BAR_COLOR;
    return GRAD->m_colors.front();
}

// Rounded outline, `width` thick, outside `box`: in `color`, else general:col.active_border
// (gradient included).
static void hyprdropAddRoundBorder(const CBox& box, double round, float roundingPower, double width, std::optional<CHyprColor> color) {
    static CConfigValue<Config::IComplexConfigValue> ACTIVEBORDER("general:col.active_border");
    const auto* GRAD = ACTIVEBORDER.good() ? dynamic_cast<Config::CGradientValueData*>(ACTIVEBORDER.ptr()) : nullptr;

    CBorderPassElement::SBorderData data;
    data.box           = box;
    data.grad1         = color ? Config::CGradientValueData{*color} : GRAD && !GRAD->m_colors.empty() ? *GRAD : Config::CGradientValueData{HYPRDROP_ACTIVE_BAR_COLOR};
    data.round         = std::round(round);
    data.roundingPower = roundingPower;
    data.borderSize    = std::max(1.0, std::round(width));
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(data));
}

// App icon of a window, from its class: <class>.desktop (or a desktop file whose
// StartupWMClass is the class) gives the icon name, looked up in hicolor and pixmaps.
// Cached by class, misses included. Logical size HYPRDROP_ICON_SIZE at most.
// ponytail: no icon theme resolution (hicolor + pixmaps only); add the user's theme if icons are missing
static constexpr double                     HYPRDROP_ICON_SIZE = 64.0;

static std::string hyprdropDesktopIconName(const std::string& cls) {
    const std::string HOME = getenv("HOME") ? getenv("HOME") : "";
    const std::vector<std::string> DIRS = {HOME + "/.local/share/applications", "/usr/share/applications"};
    const auto ICONOF = [](const std::filesystem::path& f) -> std::pair<std::string, std::string> { // icon, wm class
        std::ifstream in(f);
        std::string   line, icon, wm;
        while (std::getline(in, line)) {
            if (line.starts_with("Icon=") && icon.empty())
                icon = line.substr(5);
            else if (line.starts_with("StartupWMClass="))
                wm = line.substr(15);
        }
        return {icon, wm};
    };

    // Every filesystem call takes an error_code: an exception thrown here (unreadable
    // directory...) would go up through the render hook and take Hyprland down.
    std::error_code ec;
    std::string     lower = cls;
    std::ranges::transform(lower, lower.begin(), ::tolower);
    for (const auto& d : DIRS) {
        for (const auto& name : {cls, lower}) {
            if (const auto F = d + "/" + name + ".desktop"; std::filesystem::exists(F, ec)) {
                if (const auto ICON = ICONOF(F).first; !ICON.empty())
                    return ICON;
            }
        }
    }
    for (const auto& d : DIRS) {
        for (auto it = std::filesystem::directory_iterator(d, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (it->path().extension() != ".desktop")
                continue;
            if (const auto [ICON, WM] = ICONOF(it->path()); !ICON.empty() && WM == cls)
                return ICON;
        }
    }
    return lower; // many apps name their icon after their class
}

static SP<Render::ITexture> hyprdropIconOf(PHLWINDOW w, double monScale) {
    const auto IT = g_hyprdropIcons.find(w->m_class);
    if (IT != g_hyprdropIcons.end())
        return IT->second;

    auto&             tex  = g_hyprdropIcons[w->m_class];
    const std::string NAME = hyprdropDesktopIconName(w->m_class);
    const std::string HOME = getenv("HOME") ? getenv("HOME") : "";
    std::vector<std::string> candidates;
    if (NAME.starts_with("/"))
        candidates.push_back(NAME);
    for (const auto& base : {HOME + "/.local/share/icons/hicolor", std::string{"/usr/share/icons/hicolor"}}) {
        candidates.push_back(base + "/scalable/apps/" + NAME + ".svg");
        for (const auto* SZ : {"256x256", "512x512", "128x128", "96x96", "64x64", "48x48"})
            candidates.push_back(base + "/" + SZ + "/apps/" + NAME + ".png");
    }
    candidates.push_back("/usr/share/pixmaps/" + NAME + ".svg");
    candidates.push_back("/usr/share/pixmaps/" + NAME + ".png");

    std::error_code ec;
    for (const auto& path : candidates) {
        if (!std::filesystem::exists(path, ec))
            continue;
        const double         PX = HYPRDROP_ICON_SIZE * monScale * 2; // some margin for sharpness
        Hyprgraphics::CImage img(path, {PX, PX});
        if (!img.success() || !img.cairoSurface()) {
            dbg("icon: '" + path + "' failed to load: " + img.getError());
            continue;
        }
        tex = g_pHyprRenderer->createTexture(img.cairoSurface()->cairo());
        dbg(std::format("icon: class '{}' -> '{}'{}", w->m_class, path, tex ? "" : " (texture failed)"));
        return tex;
    }
    dbg("icon: no icon found for class '" + w->m_class + "' (name '" + NAME + "')");
    return tex;
}

// The window's icon centered on `win` (a placed window box), at most HYPRDROP_ICON_SIZE and
// at most 40% of the window's smaller side.
static void hyprdropAddIcon(PHLWINDOW w, const CBox& win, double monScale, const CBox& clip, float alpha) {
    const auto TEX = hyprdropIconOf(w, monScale);
    if (!TEX || TEX->m_size.x <= 0 || TEX->m_size.y <= 0)
        return;
    const double SIDE = std::min(HYPRDROP_ICON_SIZE, 0.4 * std::min(win.w, win.h));
    const double F    = SIDE / std::max(TEX->m_size.x, TEX->m_size.y);
    const Vector2D SZ = TEX->m_size * F;

    CTexPassElement::SRenderData data;
    data.tex    = TEX;
    data.box    = {win.x + (win.w - SZ.x) / 2, win.y + (win.h - SZ.y) / 2, SZ.x, SZ.y};
    if (!clip.empty())
        data.clipBox = CBox{clip}.scale(monScale);
    data.a      = alpha;
    data.damage = CRegion{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
}

// Workspace number in the middle of the tile, as on the keyboard: workspace 10 is "0".
// special:magic shows "Hidden". Text textures are made once and kept.
static constexpr int    HYPRDROP_LABEL_PT      = 14;

static constexpr double HYPRDROP_LABEL_INSET   = 3.0; // logical px, around the text in its box

static const CHyprColor HYPRDROP_LABEL_BG      = CHyprColor{0.0, 0.0, 0.0, 1.0};

static void hyprdropAddTileLabel(WORKSPACEID id, const CBox& tile, double monScale) {
    const std::string TEXT = id == g_hyprdropMagicID ? "Hidden" : id == 10 ? "0" : std::to_string(id);

    auto& tex = g_hyprdropLabels[TEXT];
    if (!tex) {
        static CConfigValue<Config::STRING> FONT("misc:font_family");
        Hyprgraphics::CTextResource::STextResourceData data;
        data.text     = TEXT;
        data.font     = FONT.good() ? std::string{*FONT} : "Sans";
        data.fontSize = std::round(HYPRDROP_LABEL_PT * monScale * 0.75); // this call's size comes out 4/3 larger than renderText(text, col, pt)
        data.align    = Hyprgraphics::CTextResource::TEXT_ALIGN_CENTER;
        tex           = g_pHyprRenderer->renderText(std::move(data));
    }
    if (!tex)
        return;

    // Box behind the text, centered in the tile: a square around a single digit, otherwise
    // the text plus a margin (a bit more on the sides).
    const Vector2D TS  = tex->m_size / monScale;
    const Vector2D PAD = {TS.x > TS.y ? 3 * HYPRDROP_LABEL_INSET : HYPRDROP_LABEL_INSET, HYPRDROP_LABEL_INSET};
    Vector2D       BOX = TS + PAD * 2;
    BOX.x              = std::max(BOX.x, BOX.y);
    const Vector2D SQ   = tile.pos() + (tile.size() - BOX) / 2.0;
    hyprdropAddRect({SQ.x, SQ.y, BOX.x, BOX.y}, HYPRDROP_LABEL_BG);

    CTexPassElement::SRenderData data;
    data.tex    = tex;
    data.box    = {SQ.x + (BOX.x - TS.x) / 2, SQ.y + (BOX.y - TS.y) / 2, TS.x, TS.y};
    data.damage = CRegion{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
}

// Trash icon: Phosphor Icons "trash" (regular, U+E4A6), MIT license, https://phosphoricons.com.
// Embedded as SVG so no icon font is needed; fill="currentColor" is replaced by the color.
static const std::string HYPRDROP_TRASH_SVG =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" fill="currentColor"><path d="M216,48H176V40a24,24,0,0,0-24-24H104A24,24,0,0,0,80,40v8H40a8,8,0,0,0,0,16h8V208a16,16,0,0,0,16,16H192a16,16,0,0,0,16-16V64h8a8,8,0,0,0,0-16ZM96,40a8,8,0,0,1,8-8h48a8,8,0,0,1,8,8v8H96Zm96,168H64V64H192ZM112,104v64a8,8,0,0,1-16,0V104a8,8,0,0,1,16,0Zm48,0v64a8,8,0,0,1-16,0V104a8,8,0,0,1,16,0Z"/></svg>)";
static const CHyprColor HYPRDROP_TRASH_HOT = CHyprColor{0.93, 0.26, 0.26, 1.0}; // a dragged window is over it

// Trash icon in `color`, `px` physical pixels wide. Kept by color and size.
static SP<Render::ITexture> hyprdropTrashIcon(const CHyprColor& color, int px) {
    const std::string KEY = std::format("trash:{:.3f},{:.3f},{:.3f},{:.3f}@{}", color.r, color.g, color.b, color.a, px);
    if (const auto IT = g_hyprdropIcons.find(KEY); IT != g_hyprdropIcons.end())
        return IT->second;

    std::string svg = HYPRDROP_TRASH_SVG;
    const auto  FILL = std::format(R"(fill="#{:02x}{:02x}{:02x}" fill-opacity="{:.3f}")", (int)std::round(color.r * 255), (int)std::round(color.g * 255),
                                   (int)std::round(color.b * 255), color.a);
    svg.replace(svg.find(R"(fill="currentColor")"), std::string_view{R"(fill="currentColor")"}.size(), FILL);

    Hyprgraphics::CImage img(std::span<const uint8_t>((const uint8_t*)svg.data(), svg.size()), Hyprgraphics::IMAGE_FORMAT_SVG, Vector2D{(double)px, (double)px});
    auto&                tex = g_hyprdropIcons[KEY];
    if (img.success() && img.cairoSurface())
        tex = g_pHyprRenderer->createTexture(img.cairoSurface()->cairo());
    else
        dbg("trash: icon failed to load: " + img.getError());
    return tex;
}

// The trash: an empty tile with the icon in the active border color, red with a red border
// while a dragged window is over it or the delete mode is on. Hovered by the pointer, its
// border takes the active border color like a tile, even in delete mode (the icon stays red).
static void hyprdropAddTrash(const CBox& box, double monScale) {
    const bool DROP = hyprdropDragging() && g_hyprdropTrashHover;
    const bool HOT  = DROP || g_hyprdropDeleteMode;
    hyprdropAddRect(box, HYPRDROP_TILE_BG);
    // Border: hovered by the pointer (not a drop) -> active border color, even in delete
    // mode; otherwise red when hot.
    if (g_hyprdropTrashHover && !DROP)
        hyprdropAddBorder(box, HYPRDROP_TILE_BORDER, hyprdropActiveBorderColor());
    else if (HOT)
        hyprdropAddBorder(box, HYPRDROP_TILE_BORDER, HYPRDROP_TRASH_HOT);

    const double SIDE = box.h * 0.45;
    const auto   TEX  = hyprdropTrashIcon(HOT ? HYPRDROP_TRASH_HOT : hyprdropActiveBorderColor(), std::round(SIDE * monScale));
    if (!TEX)
        return;
    CTexPassElement::SRenderData data;
    data.tex    = TEX;
    data.box    = {box.x + (box.w - SIDE) / 2, box.y + (box.h - SIDE) / 2, SIDE, SIDE};
    data.damage = CRegion{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
}

// Workspace `id` in `box`: a background (skipped if fully transparent), then its captured
// windows, bottom to top. Textures cover the whole monitor, so they are clipped to the view.
// `clip` defaults to `box`; the sliding top view draws shifted boxes clipped to the view.
static void hyprdropAddWorkspace(WORKSPACEID id, const CBox& box, double monScale, const CHyprColor& bg, std::optional<CBox> clip) {
    const CBox CLIP = clip.value_or(box);
    // Backgrounds are rounded like the windows (decoration:rounding), at the view's scale.
    static CConfigValue<Config::INTEGER> ROUNDING("decoration:rounding");
    const double ROUND = (ROUNDING.good() ? *ROUNDING : 0) * box.w / g_hyprdropCaptureUsable.w;
    if (bg.a > 0)
        hyprdropAddRect(box, bg, ROUND);

    const auto CAPS = hyprdropCaptureOf(id);
    if (!CAPS) {
        // An empty view with no background would look like a rendering bug: show it as an
        // empty tile does.
        if (bg.a <= 0)
            hyprdropAddRect(box.intersection(CLIP), HYPRDROP_TILE_BG, ROUND);
        return;
    }

    for (const auto& c : *CAPS) {
        if (!c.fb || !c.fb->getTexture())
            continue;

        CTexPassElement::SRenderData tex;
        tex.tex    = c.fb->getTexture();
        const auto PLACED = hyprdropPlace(c, box);
        tex.box     = PLACED.tex;
        tex.clipBox = CBox{CLIP}.scale(monScale); // clipBox is in scaled (physical) coordinates
        tex.damage  = CRegion{0, 0, INT16_MAX, INT16_MAX};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(tex));
        if (const auto W = c.win.lock())
            hyprdropAddIcon(W, PLACED.win, monScale, CLIP);
    }
}

// During the frame: background, large top view, bottom strip with the tiles.
void hyprdropDraw(PHLMONITOR mon) {
    if (!hyprdropVisible() || g_hyprdropCapturing || !mon)
        return;

    const Vector2D S = mon->m_size;
    const auto     L = hyprdropLayout(S);

    // Background: the wallpaper alone (the bar, a top layer, is drawn over us anyway).
    if (g_hyprdropHasWallpaper && g_hyprdropWallpaperFB && g_hyprdropWallpaperFB->getTexture()) {
        CTexPassElement::SRenderData tex;
        tex.tex    = g_hyprdropWallpaperFB->getTexture();
        tex.box    = {0, 0, S.x, S.y};
        tex.damage = CRegion{0, 0, INT16_MAX, INT16_MAX};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(tex));
    } else
        hyprdropAddRect({0, 0, S.x, S.y}, CHyprColor{0.0, 0.0, 0.0, 0.92});

    // Closing: the top view grows back onto what the monitor lands on: its workspace, with
    // the open special workspace over it, as Hyprland draws them.
    WORKSPACEID SHOWN = hyprdropShownID();
    if (!g_hyprdropOpen) {
        if (mon->m_activeWorkspace) {
            SHOWN = mon->m_activeWorkspace->m_id;
            if (mon->m_activeSpecialWorkspace)
                hyprdropAddWorkspace(SHOWN, L.top, mon->m_scale, CHyprColor{0, 0, 0, 0});
        }
        if (mon->m_activeSpecialWorkspace)
            SHOWN = mon->m_activeSpecialWorkspace->m_id;
    }

    const double SLIDE = hyprdropEase(hyprdropElapsedMs(g_hyprdropSlideStart) / HYPRDROP_SLIDE_MS);
    if (g_hyprdropOpen && SLIDE < 1.0 && g_hyprdropSlideFromID != SHOWN) {
        const double W = (L.top.w + L.top.w * 0.05) * g_hyprdropSlideDir;
        hyprdropAddWorkspace(g_hyprdropSlideFromID, CBox{L.top}.translate(Vector2D{-W * SLIDE, 0.0}), mon->m_scale, CHyprColor{0, 0, 0, 0}, L.top);
        hyprdropAddWorkspace(SHOWN, CBox{L.top}.translate(Vector2D{W * (1.0 - SLIDE), 0.0}), mon->m_scale, CHyprColor{0, 0, 0, 0}, L.top);
    } else
        hyprdropAddWorkspace(SHOWN, L.top, mon->m_scale, CHyprColor{0, 0, 0, 0});

    // Window under the pointer in the top view (no drag): outlined like the shown tile.
    if (g_hyprdropOpen && SLIDE >= 1.0 && !hyprdropDragging() && L.top.containsPoint(g_hyprdropPointer)) {
        if (const auto HIT = hyprdropCapturedWindowAt(SHOWN, L.top, g_hyprdropPointer)) {
            const auto& [W, PLACED] = *HIT;
            const auto*  CAP        = hyprdropFindWindowCapture(W);
            const double F          = CAP && CAP->box.w > 0 ? PLACED.win.w / CAP->box.w : 1.0;
            // At least as thick as the window's own border (captured with it, in the active
            // or inactive color), so it covers it instead of drawing a second color around it.
            const double WIDTH = std::max(HYPRDROP_TILE_BORDER, W->getRealBorderSize() * F);
            hyprdropAddRoundBorder(PLACED.win, W->rounding() * F, W->roundingPower(), WIDTH,
                                   g_hyprdropDeleteMode ? std::optional{HYPRDROP_TRASH_HOT} : std::nullopt); // red: a click closes it
        }
    }

    // Strip: a dark veil over the blurred wallpaper.
    CRectPassElement::SRectData strip;
    strip.box   = L.strip;
    strip.color = HYPRDROP_STRIP_COLOR;
    strip.blur  = true;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(strip));

    for (size_t i = 0; i < L.tiles.size(); ++i) {
        // Border: active border color on the tile shown in the top view; white on the
        // tile a drag is over.
        const bool ISSHOWN = g_hyprdropTiles[i] == SHOWN;
        const bool ISDROP  = hyprdropDragging() && (int)i == g_hyprdropHover;
        hyprdropAddWorkspace(g_hyprdropTiles[i], L.tiles[i], mon->m_scale, HYPRDROP_TILE_BG);
        if (ISSHOWN || ISDROP)
            hyprdropAddBorder(L.tiles[i], HYPRDROP_TILE_BORDER, ISDROP ? HYPRDROP_DROP_BORDER : hyprdropActiveBorderColor());
        hyprdropAddTileLabel(g_hyprdropTiles[i], L.tiles[i], mon->m_scale);
    }

    if (!L.activeBar.empty())
        hyprdropAddRect(L.activeBar, hyprdropActiveBorderColor());

    if (!L.trash.empty())
        hyprdropAddTrash(L.trash, mon->m_scale);

    // Ghost of the dragged window, on top of everything else: its captured image, slightly
    // transparent, framed with the active border color so it stands out from the windows
    // of the top view. A plain rectangle if the window has no capture.
    if (const auto WIN = hyprdropDragging() ? g_hyprdropDragWin.lock() : nullptr) {
        // Scaled by HYPRDROP_GHOST_SCALE, still held at the same relative point. Sized from
        // the live capture, so it follows the window when a preview placement resizes it.
        const auto* CAP  = hyprdropFindWindowCapture(WIN);
        Vector2D    SIZE = g_hyprdropDragSize;
        if (CAP && CAP->box.w > 0 && CAP->box.h > 0)
            SIZE = CAP->box.size() * hyprdropFitOf(CAP->ws).scale * (L.top.w / g_hyprdropCaptureUsable.w);
        SIZE                 = SIZE * HYPRDROP_GHOST_SCALE;
        const Vector2D POS   = g_hyprdropPointer - SIZE / 2.0; // centered on the pointer
        const CBox     GHOST = {POS.x, POS.y, SIZE.x, SIZE.y};

        if (!CAP || !CAP->fb || !CAP->fb->getTexture() || CAP->box.w <= 0 || CAP->box.h <= 0) {
            hyprdropAddRect(GHOST, CHyprColor{1.0, 1.0, 1.0, 0.35});
            return;
        }


        // The texture covers the whole monitor: scale it so the window's part fills the
        // ghost, and clip to the ghost.
        const Vector2D Q = {GHOST.w / CAP->box.w, GHOST.h / CAP->box.h};
        CTexPassElement::SRenderData tex;
        tex.tex     = CAP->fb->getTexture();
        tex.box     = CBox{GHOST.pos() + (-g_hyprdropCaptureUsable.pos() - CAP->box.pos()) * Q, g_hyprdropCaptureMonSize * Q};
        tex.clipBox = CBox{GHOST}.scale(mon->m_scale);
        tex.a       = HYPRDROP_GHOST_ALPHA;
        tex.damage  = CRegion{0, 0, INT16_MAX, INT16_MAX};
        // Same rounding as the window, scaled like it.
        const double ROUND  = WIN->rounding() * GHOST.w / CAP->box.w;
        tex.round         = std::round(ROUND);
        tex.roundingPower = WIN->roundingPower();
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(tex));
        hyprdropAddRoundBorder(GHOST, ROUND, WIN->roundingPower(), HYPRDROP_GHOST_FRAME);
        hyprdropAddIcon(WIN, GHOST, mon->m_scale, {}, HYPRDROP_GHOST_ALPHA);
    }
}

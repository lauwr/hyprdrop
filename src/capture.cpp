#include "hyprdrop.hpp"

static bool hyprdropInStripRange(WORKSPACEID id);
static void hyprdropRebuildTiles();
static void hyprdropRenderWindow(HyprdropFB& fb, PHLMONITOR mon, PHLWINDOW w);
static Vector2D hyprdropGapsIn(PHLWORKSPACE ws);
static void hyprdropCaptureLog(const std::string& s);
static void hyprdropCaptureWorkspace(PHLMONITOR mon, PHLWORKSPACE ws, std::vector<HyprdropFB>& pool);
static void hyprdropCaptureWallpaper(PHLMONITOR mon);

// The strip always shows workspaces 1..HYPRDROP_STRIP_WORKSPACES, including the ones that
// don't exist yet (drawn as empty tiles).
static constexpr WORKSPACEID HYPRDROP_STRIP_WORKSPACES = 10;

// Special workspaces other than special:magic: every one ever seen, by name -> id. They
// keep their tile once empty (Hyprland destroys empty special workspaces), like the
// numbered ones; a drop recreates them. The names are saved in
// $XDG_STATE_HOME/hyprdrop/specials (default ~/.local/state), one per line, so they
// survive reloads; delete a line to drop a tile.
static std::map<std::string, WORKSPACEID> g_hyprdropKnownSpecials;
static std::vector<WORKSPACEID>           g_hyprdropOtherSpecials; // their ids, by name

static std::filesystem::path hyprdropSpecialsFile() {
    const char*        STATE = getenv("XDG_STATE_HOME");
    const std::string  HOME  = getenv("HOME") ? getenv("HOME") : "";
    return std::filesystem::path(STATE && *STATE ? STATE : HOME + "/.local/state") / "hyprdrop" / "specials";
}

// Reads the saved names once (their ids are given at capture).
static void hyprdropLoadSpecials() {
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;
    std::ifstream in(hyprdropSpecialsFile());
    for (std::string name; std::getline(in, name);) {
        if (name.starts_with("special:") && name != HYPRDROP_MAGIC_NAME && !g_hyprdropKnownSpecials.contains(name))
            g_hyprdropKnownSpecials[name] = WORKSPACE_INVALID;
    }
}

static void hyprdropSaveSpecials() {
    std::error_code ec;
    std::filesystem::create_directories(hyprdropSpecialsFile().parent_path(), ec);
    std::ofstream out(hyprdropSpecialsFile(), std::ios::trunc);
    for (const auto& [name, id] : g_hyprdropKnownSpecials)
        out << name << "\n";
}

// ---------------------------------------------------------------- workspaces

// Existing workspace with this id, or nullptr.
PHLWORKSPACE hyprdropFindWorkspace(WORKSPACEID id) {
    for (const auto& w : State::workspaceState()->workspacesCopy()) {
        if (w && !w->inert() && w->m_id == id)
            return w;
    }
    return nullptr;
}

PHLWORKSPACE hyprdropFindWorkspaceByName(const std::string& name) {
    for (const auto& w : State::workspaceState()->workspacesCopy()) {
        if (w && !w->inert() && w->m_name == name)
            return w;
    }
    return nullptr;
}

// Name used when creating workspace `id`.
std::string hyprdropWorkspaceName(WORKSPACEID id) {
    if (id == g_hyprdropMagicID)
        return HYPRDROP_MAGIC_NAME;
    for (const auto& [name, knownID] : g_hyprdropKnownSpecials) {
        if (knownID == id)
            return name;
    }
    return std::to_string(id);
}

// Finding the window under the cursor in the top view assumes the windows of D have
// positions on `mon`, as if D were shown there. Logs whatever breaks that assumption,
// which matters most for workspaces that aren't shown (special:magic in particular).
void hyprdropCheckPositions(PHLMONITOR mon, PHLWORKSPACE ws) {
    if (!ws)
        return;

    const auto WSMON = ws->m_monitor.lock();
    if (WSMON != mon)
        dbg(std::format("positions: workspace {} ({}) is on monitor {}, not on {}: its windows may be placed elsewhere", ws->m_id, ws->m_name,
                        WSMON ? WSMON->m_name : "none", mon->m_name));

    const CBox MONBOX = {mon->m_position, mon->m_size};
    int        total = 0, outside = 0;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (!w || !w->m_isMapped || w->m_workspace != ws)
            continue;
        ++total;
        const CBox B = w->getWindowMainSurfaceBox();
        if (B.w <= 0 || B.h <= 0 || B.x >= MONBOX.x + MONBOX.w || B.y >= MONBOX.y + MONBOX.h || B.x + B.w <= MONBOX.x || B.y + B.h <= MONBOX.y) {
            ++outside;
            dbg(std::format("positions: '{}' on workspace {} has box ({:.0f},{:.0f} {:.0f}x{:.0f}) outside the monitor ({:.0f},{:.0f} {:.0f}x{:.0f})", w->m_title,
                            ws->m_id, B.x, B.y, B.w, B.h, MONBOX.x, MONBOX.y, MONBOX.w, MONBOX.h));
        }
    }
    dbg(std::format("positions: workspace {} ({}): {} windows, {} outside the monitor{}", ws->m_id, ws->m_name, total, outside,
                    outside ? " -> picking a window in the top view will be wrong" : ""));
}

const std::vector<SHyprdropWinCapture>* hyprdropCaptureOf(WORKSPACEID id) {
    const auto IT = g_hyprdropCaptures.find(id);
    return IT == g_hyprdropCaptures.end() || IT->second.empty() ? nullptr : &IT->second;
}

static bool hyprdropInStripRange(WORKSPACEID id) {
    return id >= 1 && id <= HYPRDROP_STRIP_WORKSPACES;
}

// Bottom strip = 1..HYPRDROP_STRIP_WORKSPACES, sorted by id, plus A at the end if it is
// outside that range. D keeps its tile (drawn greyed), so the strip never moves when D
// changes. special:magic always comes last: the layout puts that last tile on the second row.
static void hyprdropRebuildTiles() {
    g_hyprdropTiles.clear();
    for (WORKSPACEID id = 1; id <= HYPRDROP_STRIP_WORKSPACES; ++id)
        g_hyprdropTiles.push_back(id);
    if (g_hyprdropActiveID != WORKSPACE_INVALID && !hyprdropInStripRange(g_hyprdropActiveID))
        g_hyprdropTiles.push_back(g_hyprdropActiveID);
    if (g_hyprdropMagicID != WORKSPACE_INVALID)
        g_hyprdropTiles.push_back(g_hyprdropMagicID);
    for (const auto ID : g_hyprdropOtherSpecials)
        g_hyprdropTiles.push_back(ID);
}

// Some tile's workspace lives on another monitor than the overview's: its name gets a
// second line (the monitor), so the layout leaves room for it.
bool hyprdropAnyTileOnOtherMonitor() {
    const auto MON = hyprdropMonitor();
    for (const auto ID : g_hyprdropTiles) {
        const auto WS = hyprdropFindWorkspace(ID);
        if (WS && WS->m_monitor.lock() && WS->m_monitor.lock() != MON)
            return true;
    }
    return false;
}

// Number of special workspace tiles, at the end of the strip (its second row).
int hyprdropSpecialTiles() {
    int n = 0;
    for (auto it = g_hyprdropTiles.rbegin(); it != g_hyprdropTiles.rend() && hyprdropIsSpecialID(*it); ++it)
        ++n;
    return n;
}

// Index of A's tile in the strip, or -1.
int hyprdropActiveTile() {
    const auto IT = std::ranges::find(g_hyprdropTiles, g_hyprdropActiveID);
    return IT == g_hyprdropTiles.end() ? -1 : (int)(IT - g_hyprdropTiles.begin());
}

// ---------------------------------------------------------------- capture

// Renders window `w` alone into `fb` (monitor-sized), at its real position, on a
// transparent background. Same call as hyprwinwrap: decorated, all passes (popups too).
static void hyprdropRenderWindow(HyprdropFB& fb, PHLMONITOR mon, PHLWINDOW w) {
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    if (!fb)
        fb = g_pHyprRenderer->createFB("hyprdrop");

    const CBox MONBOX{{0, 0}, mon->m_pixelSize};
    if (fb->m_size != MONBOX.size()) {
        fb->release();
        fb->alloc(MONBOX.w, MONBOX.h, DRM_FORMAT_ABGR8888);
    }

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    if (!g_pHyprRenderer->beginRender(mon, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, fb)) {
        hyprdropCaptureLog("capture: beginRender failed for '" + w->m_title + "'");
        return; // no endRender without a begun render
    }

    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    g_pHyprRenderer->renderWindow(w, mon, Time::steadyNow(), true, Render::RENDER_PASS_ALL, false, false);

    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
}

// gaps_in of workspace `ws` (x = left/right, y = top/bottom, logical px): its workspace
// rule if it sets one, else general:gaps_in.
static Vector2D hyprdropGapsIn(PHLWORKSPACE ws) {
    if (const auto RULE = Config::workspaceRuleMgr()->getWorkspaceRuleFor(ws); RULE && RULE->m_gapsIn)
        return {(double)RULE->m_gapsIn->m_left, (double)RULE->m_gapsIn->m_top};

    static CConfigValue<Config::IComplexConfigValue> GAPSIN("general:gaps_in");
    if (GAPSIN.good()) {
        if (const auto* G = dynamic_cast<Config::CCssGapData*>(GAPSIN.ptr()))
            return {(double)G->m_left, (double)G->m_top};
    }
    return {0, 0};
}

static void hyprdropCaptureLog(const std::string& s) {
    if (!g_hyprdropLive)
        dbg(s);
}

// Usable area of `mon` (monitor minus the space reserved by bars), logical, monitor-local.
static CBox hyprdropUsableOf(PHLMONITOR mon) {
    const auto& R = mon->m_reservedArea;
    const CBox  U = {R.left(), R.top(), mon->m_size.x - R.left() - R.right(), mon->m_size.y - R.top() - R.bottom()};
    return U.w > 0 && U.h > 0 ? U : CBox{0, 0, mon->m_size.x, mon->m_size.y};
}

// `mon` is the overview's monitor. A workspace living on another monitor is rendered on
// that one (its positions, size and scale), then fitted into the overview's views.
static void hyprdropCaptureWorkspace(PHLMONITOR mon, PHLWORKSPACE ws, std::vector<HyprdropFB>& pool) {
    const PHLMONITOR OVMON = mon;
    if (const auto WSMON = ws->m_monitor.lock())
        mon = WSMON;
    const CBox USABLE = mon == OVMON ? g_hyprdropCaptureUsable : hyprdropUsableOf(mon);

    // Drawing order: tiled then floating, each in stack order (bottom to top).
    std::vector<PHLWINDOW> windows;
    for (bool floating : {false, true}) {
        for (const auto& w : Desktop::windowState()->windows()) {
            if (w && w->m_isMapped && !w->isHidden() && w->m_workspace == ws && w->m_isFloating == floating)
                windows.push_back(w);
        }
    }

    if (windows.empty()) {
        hyprdropCaptureLog(std::format("capture: workspace {} ({}) has no windows, empty tile", ws->m_id, ws->m_name));
        return;
    }

    const PHLWORKSPACE ACTIVE     = mon->m_activeWorkspace;
    const PHLWORKSPACE SPECIAL    = mon->m_activeSpecialWorkspace;
    const bool         ISSPECIAL  = ws->m_isSpecialWorkspace;
    const bool         SWAPNORMAL = !ISSPECIAL && ws != ACTIVE;
    const bool         WASVISIBLE = ws->m_visible;

    if (ISSPECIAL) {
        mon->m_activeSpecialWorkspace = ws;
        Animation::Workspace::startAnimation(ws, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
        ws->m_visible = true;
    } else if (SWAPNORMAL) {
        if (SPECIAL)
            mon->m_activeSpecialWorkspace.reset();
        if (ACTIVE)
            ACTIVE->m_visible = false;
        mon->m_activeWorkspace = ws;
        Animation::Workspace::startAnimation(ws, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
        ws->m_visible = true;
    }

    auto& caps = g_hyprdropCaptures[ws->m_id];
    for (const auto& w : windows) {
        HyprdropFB fb;
        if (!pool.empty()) {
            fb = pool.back();
            pool.pop_back();
        }
        hyprdropRenderWindow(fb, mon, w);

        CBox box = w->getWindowMainSurfaceBox();
        box.x -= mon->m_position.x + USABLE.x;
        box.y -= mon->m_position.y + USABLE.y;
        caps.push_back({w, fb, box, ws->m_id, mon->m_size, USABLE.pos()});
        if (const auto SURF = w->wlSurface() ? w->wlSurface()->resource() : nullptr)
            hyprdropCaptureLog(std::format("capture: '{}' window {:.0f}x{:.0f}, client surface {:.0f}x{:.0f} (buffer {:.0f}x{:.0f}), reported {:.0f}x{:.0f}, pending {:.0f}x{:.0f}, {} acks waiting{}", w->m_title, box.w, box.h,
                                           SURF->m_current.size.x, SURF->m_current.size.y, SURF->m_current.bufferSize.x, SURF->m_current.bufferSize.y,
                                           w->m_reportedSize.x, w->m_reportedSize.y, w->m_pendingReportedSize.x, w->m_pendingReportedSize.y, w->m_pendingSizeAcks.size(),
                                           std::abs(SURF->m_current.size.x - box.w) > 1 || std::abs(SURF->m_current.size.y - box.h) > 1 ? " MISMATCH" : ""));
        hyprdropCaptureLog(std::format("capture: workspace {} window '{}' at ({:.0f},{:.0f} {:.0f}x{:.0f}){}", ws->m_id, w->m_title, box.x, box.y, box.w, box.h,
                        w->m_isFloating ? " floating" : ""));
    }

    // Windows of a hidden workspace are suspended (xdg-shell) and get no frame callbacks:
    // their client stops drawing, so after a resize the old buffer stays, stretched, and
    // videos freeze. While the overview is open they are woken up and get frame callbacks;
    // closing gives the suspended states back to Hyprland.
    const auto NOW = Time::steadyNow();
    for (const auto& w : windows) {
        if (w->m_suspended) {
            hyprdropCaptureLog("capture: '" + w->m_title + "' was suspended, waking it up");
            w->setSuspended(false);
        }
        if (const auto SURF = w->wlSurface() ? w->wlSurface()->resource() : nullptr)
            SURF->breadthfirst([NOW](SP<CWLSurfaceResource> s, const Vector2D&, void*) { s->frame(NOW); }, nullptr);
    }

    // Special workspace: fit its windows' bounding box to the usable area.
    g_hyprdropFits.erase(ws->m_id);
    SHyprdropFit fit;
    fit.origin = mon->m_position + USABLE.pos();
    // Another monitor: its usable area scaled to fit the overview's, centered.
    if (mon != OVMON) {
        const Vector2D U = g_hyprdropCaptureUsable.size(), UW = USABLE.size();
        const double   F = std::min(U.x / UW.x, U.y / UW.y);
        fit.scale        = F;
        fit.offset       = (U - UW * F) / 2.0;
        hyprdropCaptureLog(std::format("capture: workspace {} is on monitor {}, fitted x{:.2f}", ws->m_id, mon->m_name, F));
    }
    if (ISSPECIAL && !caps.empty()) {
        CBox bb = caps.front().box;
        for (const auto& c : caps) {
            const double X2 = std::max(bb.x + bb.w, c.box.x + c.box.w), Y2 = std::max(bb.y + bb.h, c.box.y + c.box.h);
            bb.x = std::min(bb.x, c.box.x);
            bb.y = std::min(bb.y, c.box.y);
            bb.w = X2 - bb.x;
            bb.h = Y2 - bb.y;
        }
        const Vector2D U = g_hyprdropCaptureUsable.size();
        if (bb.w > 0 && bb.h > 0) {
            const double F = std::min(U.x / bb.w, U.y / bb.h);
            fit.scale      = F;
            fit.offset     = -bb.pos() * F + (U - bb.size() * F) / 2.0;
            hyprdropCaptureLog(std::format("capture: workspace {} fitted to the view, x{:.2f}", ws->m_id, F));
        }
    }

    // Gaps: what is already there (2 x gaps_in, scaled by the fit) is not added again.
    const Vector2D GAPSIN = hyprdropGapsIn(ws);
    const double   TARGET = (2 * HYPRDROP_GAPS_IN_REF + HYPRDROP_WINDOW_GAP) / g_hyprdropCaptureScale;
    fit.gap               = {std::max(0.0, TARGET - 2 * GAPSIN.x * fit.scale), std::max(0.0, TARGET - 2 * GAPSIN.y * fit.scale)};
    g_hyprdropFits[ws->m_id] = fit;
    hyprdropCaptureLog(std::format("capture: workspace {} gaps_in {:.0f}x{:.0f}, extra gap {:.0f}x{:.0f}", ws->m_id, GAPSIN.x, GAPSIN.y, fit.gap.x, fit.gap.y));

    if (ISSPECIAL) {
        ws->m_visible                 = WASVISIBLE;
        mon->m_activeSpecialWorkspace = SPECIAL;
        if (!WASVISIBLE)
            Animation::Workspace::startAnimation(ws, Animation::Workspace::ANIMATION_TYPE_OUT, false, true);
    } else if (SWAPNORMAL) {
        // Restored as it was: a workspace shown on another monitor must stay visible there.
        ws->m_visible = WASVISIBLE;
        if (!WASVISIBLE)
            Animation::Workspace::startAnimation(ws, Animation::Workspace::ANIMATION_TYPE_OUT, false, true);
        mon->m_activeSpecialWorkspace = SPECIAL;
        mon->m_activeWorkspace        = ACTIVE;
        if (ACTIVE) {
            ACTIVE->m_visible = true;
            Animation::Workspace::startAnimation(ACTIVE, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
        }
    }
}

static void hyprdropCaptureWallpaper(PHLMONITOR mon) {
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    if (!g_hyprdropWallpaperFB)
        g_hyprdropWallpaperFB = g_pHyprRenderer->createFB("hyprdrop-wallpaper");

    const CBox MONBOX{{0, 0}, mon->m_pixelSize};
    if (g_hyprdropWallpaperFB->m_size != MONBOX.size()) {
        g_hyprdropWallpaperFB->release();
        g_hyprdropWallpaperFB->alloc(MONBOX.w, MONBOX.h, DRM_FORMAT_ABGR8888);
    }

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    if (!g_pHyprRenderer->beginRender(mon, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, g_hyprdropWallpaperFB)) {
        dbg("capture: beginRender failed for the wallpaper");
        g_hyprdropHasWallpaper = false;
        return;
    }
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    int layers = 0;
    for (const auto& ref : mon->m_layerSurfaceLayers[0]) { // 0 = background layer
        if (const auto LS = ref.lock()) {
            g_pHyprRenderer->renderLayer(LS, mon, Time::steadyNow());
            ++layers;
        }
    }

    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();

    g_hyprdropHasWallpaper = layers > 0;
    dbg(std::format("capture: wallpaper, {} background layer(s)", layers));
}

// Captures workspace `id` again (its old framebuffers are reused). Used on every frame.
void hyprdropRecaptureWorkspace(PHLMONITOR mon, WORKSPACEID id) {
    std::vector<HyprdropFB> pool;
    if (const auto IT = g_hyprdropCaptures.find(id); IT != g_hyprdropCaptures.end()) {
        for (auto& c : IT->second) {
            if (c.fb)
                pool.push_back(c.fb);
        }
        g_hyprdropCaptures.erase(IT);
    }

    const auto WS = hyprdropFindWorkspace(id);
    if (!WS) {
        hyprdropCaptureLog("recapture: workspace " + std::to_string(id) + " doesn't exist anymore");
        return;
    }

    g_hyprdropCapturing = true;
    hyprdropCaptureWorkspace(mon, WS, pool);
    g_hyprdropCapturing = false;
}

// First capture when the overview opens: wallpaper, usable area, A, D, then the active
// workspace, every existing workspace the strip can show, and special:magic. Never
// creates a workspace: missing ones simply get no capture.
void hyprdropCapture(PHLMONITOR mon) {
    dbg("capture: start");
    g_hyprdropCapturing = true;

    // Framebuffers of the previous capture are reused; leftovers are released.
    std::vector<HyprdropFB> pool;
    for (auto& [id, caps] : g_hyprdropCaptures) {
        for (auto& c : caps) {
            if (c.fb)
                pool.push_back(c.fb);
        }
    }
    g_hyprdropCaptures.clear();
    g_hyprdropCaptureScale = mon->m_scale > 0 ? mon->m_scale : 1.0;

    // Usable area from the reserved area (left/top/right/bottom, logical px); the whole
    // monitor if nothing is left.
    const auto& R           = mon->m_reservedArea;
    g_hyprdropCaptureUsable = hyprdropUsableOf(mon);
    const CBox MINUS        = mon->logicalBoxMinusReserved();
    dbg(std::format("capture: monitor {} at ({:.0f},{:.0f}) {:.0f}x{:.0f} scale {:.2f}, reserved left {:.0f} top {:.0f} right {:.0f} bottom {:.0f}", mon->m_name,
                    mon->m_position.x, mon->m_position.y, mon->m_size.x, mon->m_size.y, g_hyprdropCaptureScale, R.left(), R.top(), R.right(), R.bottom()));
    dbg(std::format("capture: usable area (monitor-local) ({:.0f},{:.0f} {:.0f}x{:.0f}); logicalBoxMinusReserved() = ({:.0f},{:.0f} {:.0f}x{:.0f})",
                    g_hyprdropCaptureUsable.x, g_hyprdropCaptureUsable.y, g_hyprdropCaptureUsable.w, g_hyprdropCaptureUsable.h, MINUS.x, MINUS.y, MINUS.w, MINUS.h));

    hyprdropCaptureWallpaper(mon);

    const auto ACTIVE  = mon->m_activeWorkspace;
    g_hyprdropActiveID = ACTIVE ? ACTIVE->m_id : WORKSPACE_INVALID;

    const auto MAGIC  = hyprdropFindWorkspaceByName(HYPRDROP_MAGIC_NAME);
    g_hyprdropMagicID = MAGIC ? MAGIC->m_id : getWorkspaceIDNameFromString(HYPRDROP_MAGIC_NAME).id;
    dbg(std::format("capture: {} {} (id {})", HYPRDROP_MAGIC_NAME, MAGIC ? "exists" : "doesn't exist yet", g_hyprdropMagicID));

    g_hyprdropTopID = g_hyprdropOpenMode == HYPRDROP_OPEN_MAGIC ? g_hyprdropMagicID : g_hyprdropActiveID;
    dbg(std::format("capture: mode {}, D = {}, A = {}", g_hyprdropOpenMode == HYPRDROP_OPEN_MAGIC ? "magic" : "current", g_hyprdropTopID, g_hyprdropActiveID));

    if (ACTIVE) {
        dbg("capture active: workspace " + std::to_string(ACTIVE->m_id));
        hyprdropCaptureWorkspace(mon, ACTIVE, pool);
    }

    for (WORKSPACEID id = 1; id <= HYPRDROP_STRIP_WORKSPACES; ++id) {
        if (id == g_hyprdropActiveID)
            continue;
        const auto WS = hyprdropFindWorkspace(id);
        if (!WS) {
            dbg("capture other: workspace " + std::to_string(id) + " doesn't exist, empty tile");
            continue;
        }
        dbg("capture other: workspace " + std::to_string(id));
        hyprdropCaptureWorkspace(mon, WS, pool);
    }

    if (MAGIC) {
        dbg("capture magic: workspace " + std::to_string(MAGIC->m_id));
        hyprdropCheckPositions(mon, MAGIC);
        hyprdropCaptureWorkspace(mon, MAGIC, pool);
    }

    // The other special workspaces: the existing ones are remembered (and saved if new),
    // then every known one gets a tile (by name), captured if it exists.
    hyprdropLoadSpecials();
    bool newName = false;
    for (const auto& w : State::workspaceState()->workspacesCopy()) {
        if (w && !w->inert() && w->m_isSpecialWorkspace && w->m_name != HYPRDROP_MAGIC_NAME) {
            newName = newName || !g_hyprdropKnownSpecials.contains(w->m_name);
            g_hyprdropKnownSpecials[w->m_name] = w->m_id;
        }
    }
    if (newName)
        hyprdropSaveSpecials();

    // A special workspace that doesn't exist has no id: each gets a free one, distinct from
    // the existing workspaces, special:magic's and each other (newSpecialID() alone would
    // give them all the same). A drop recreates it with that id.
    std::set<WORKSPACEID> used = {g_hyprdropMagicID};
    for (const auto& w : State::workspaceState()->workspacesCopy()) {
        if (w)
            used.insert(w->m_id);
    }
    for (auto& [name, id] : g_hyprdropKnownSpecials) {
        if (const auto WS = hyprdropFindWorkspaceByName(name)) {
            id = WS->m_id;
            continue;
        }
        if (id == WORKSPACE_INVALID || used.contains(id)) {
            id = State::workspaceState()->newSpecialID();
            while (used.contains(id))
                ++id;
        }
        used.insert(id);
    }
    g_hyprdropOtherSpecials.clear();
    for (const auto& [name, id] : g_hyprdropKnownSpecials) {
        g_hyprdropOtherSpecials.push_back(id);
        if (const auto WS = hyprdropFindWorkspace(id)) {
            dbg("capture special: workspace " + std::to_string(id) + " (" + name + ")");
            hyprdropCaptureWorkspace(mon, WS, pool);
        } else
            dbg("capture special: " + name + " doesn't exist anymore, empty tile");
    }

    if (g_hyprdropOpenMode == HYPRDROP_OPEN_MAGIC && !hyprdropCaptureOf(g_hyprdropMagicID)) {
        dbg("capture: " + HYPRDROP_MAGIC_NAME + " is empty, D = A");
        g_hyprdropTopID = g_hyprdropActiveID;
    }

    hyprdropRebuildTiles();

    size_t windows = 0;
    for (const auto& [id, caps] : g_hyprdropCaptures)
        windows += caps.size();
    dbg(std::format("capture: end ({} windows in {} workspaces, {} framebuffers released, {} tiles, active workspace {} on tile {})", windows, g_hyprdropCaptures.size(),
                    pool.size(), g_hyprdropTiles.size(), g_hyprdropActiveID, hyprdropActiveTile()));
    g_hyprdropCapturing = false;

    hyprdropSizeSuperDragGhost(mon);
}

// Capture of window WIN, in whichever workspace it was captured.
const SHyprdropWinCapture* hyprdropFindWindowCapture(PHLWINDOW WIN) {
    for (const auto& [id, caps] : g_hyprdropCaptures) {
        for (const auto& c : caps) {
            if (c.win.lock() == WIN)
                return &c;
        }
    }
    return nullptr;
}

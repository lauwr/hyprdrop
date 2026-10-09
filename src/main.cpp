#include "hyprdrop.hpp"

static int luaToggle(lua_State*);
static int luaToggleCurrent(lua_State*);
static int luaDrag(lua_State*);
static PHLWORKSPACE hyprdropMagicOpenOn(PHLMONITOR mon);
static int luaGestureUp(lua_State*);
static int luaGestureDown(lua_State*);
static void hyprdropPreRender(PHLMONITOR mon);
static void hyprdropPreRenderLive(PHLMONITOR mon);

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static bool                                                    g_hyprdropNeedCapture = false;

// ---------------------------------------------------------------- Lua + rendering

// The overview's monitor while it is visible, else the focused one (where it would open).
PHLMONITOR hyprdropMonitor() {
    if (hyprdropVisible()) {
        if (const auto MON = g_hyprdropMon.lock())
            return MON;
    }
    return Desktop::focusState()->monitor();
}

// Opens the overview with D chosen by `mode`, or closes it if it is open.
void hyprdropToggle(eHyprdropOpenMode mode) {
    if (!g_hyprdropOpen)
        g_hyprdropMon = Desktop::focusState()->monitor(); // opens on the focused monitor
    g_hyprdropOpen = !g_hyprdropOpen;
    hyprdropStartOpenAnim();
    if (g_hyprdropOpen)
        g_hyprdropOpenMode = mode;
    dbg(std::string("toggle -> ") + (g_hyprdropOpen ? (mode == HYPRDROP_OPEN_MAGIC ? "open (D = special:magic)" : "open (D = A)") : "closed"));
    hyprdropSetCursorOverride(g_hyprdropOpen);

    g_hyprdropHover = -1;
    if (!g_hyprdropOpen) // closing in the middle of a drag: undo the preview placements
        hyprdropRestorePreview(hyprdropMonitor(), g_hyprdropDragWin.lock());
    hyprdropCancelDrag();
    hyprdropCancelPendingClick("overview toggled");
    g_hyprdropSuperDrag = false;
    g_hyprdropDwellID   = WORKSPACE_INVALID;
    g_hyprdropBeforeMagicID = WORKSPACE_INVALID;
    g_hyprdropTileTapID     = WORKSPACE_INVALID;
    g_hyprdropDeleteMode    = false;
    if (g_hyprdropOpen) {
        g_hyprdropNeedCapture = true;
        hyprdropRememberOpenKey();
        // A lost touch up must not leave the mouse ignored for the whole session.
        g_hyprdropTouchID = -1;
    }

    const auto MON = hyprdropMonitor();
    if (MON)
        g_pHyprRenderer->damageMonitor(MON);
}

// hl.plugin.hyprdrop.toggle_hidden(): opens with D = special:magic (fetch a window back), or closes.
static int luaToggle(lua_State*) {
    hyprdropToggle(HYPRDROP_OPEN_MAGIC);
    return 0;
}

// hl.plugin.hyprdrop.toggle_current(): opens with D = A (the current workspace), or closes.
static int luaToggleCurrent(lua_State*) {
    hyprdropToggle(HYPRDROP_OPEN_CURRENT);
    return 0;
}

// hl.plugin.hyprdrop.drag(): bind it to a mouse button (e.g. Super + left button) to drag
// the window under the cursor in the overview.
static int luaDrag(lua_State*) {
    hyprdropStartDrag();
    return 0;
}

// Up / down gestures (hl.gesture in hyprland.lua calls gesture_up() / gesture_down()).
// special:magic counts as open however it was opened (gesture, bind, click...):
//   closed, magic closed + up   -> open the overview (like toggle_hidden())
//   closed, magic open   + up   -> close magic, open the overview on the workspace under it
//   open                 + up   -> close it and open special:magic
//   open                 + down -> close the overview
//   closed, magic open   + down -> close special:magic, back to the workspace under it
static PHLWORKSPACE hyprdropMagicOpenOn(PHLMONITOR mon) {
    const auto SPECIAL = mon ? mon->m_activeSpecialWorkspace : nullptr;
    return SPECIAL && SPECIAL->m_name == HYPRDROP_MAGIC_NAME ? SPECIAL : nullptr;
}

static int luaGestureUp(lua_State*) {
    const auto MON = hyprdropMonitor();
    if (!MON)
        return 0;

    if (!g_hyprdropOpen) {
        if (const auto MAGIC = hyprdropMagicOpenOn(MON)) {
            dbg("gesture up: " + HYPRDROP_MAGIC_NAME + " is open, closing it and opening on the workspace under it");
            if (const auto RES = Config::Actions::toggleSpecial(MAGIC); !RES)
                dbg("gesture up: closing " + HYPRDROP_MAGIC_NAME + " failed: " + RES.error().message);
            hyprdropToggle(HYPRDROP_OPEN_CURRENT);
            return 0;
        }
        dbg("gesture up: opening");
        hyprdropToggle(HYPRDROP_OPEN_MAGIC);
        return 0;
    }

    dbg("gesture up: overview open, closing and opening " + HYPRDROP_MAGIC_NAME);
    hyprdropClose(MON);

    auto MAGIC = hyprdropFindWorkspaceByName(HYPRDROP_MAGIC_NAME);
    if (!MAGIC) {
        const auto ID = getWorkspaceIDNameFromString(HYPRDROP_MAGIC_NAME).id;
        MAGIC         = State::workspaceState()->create(ID, MON->m_id, HYPRDROP_MAGIC_NAME, true);
        dbg("gesture up: " + HYPRDROP_MAGIC_NAME + (MAGIC ? " created" : " could not be created"));
        if (!MAGIC)
            return 0;
    }
    if (hyprdropMagicOpenOn(MON)) {
        dbg("gesture up: " + HYPRDROP_MAGIC_NAME + " already open");
        return 0;
    }
    if (const auto RES = Config::Actions::toggleSpecial(MAGIC); !RES)
        dbg("gesture up: opening " + HYPRDROP_MAGIC_NAME + " failed: " + RES.error().message);
    return 0;
}

static int luaGestureDown(lua_State*) {
    const auto MON = hyprdropMonitor();
    if (!MON)
        return 0;

    if (g_hyprdropOpen) {
        dbg("gesture down: closing the overview");
        hyprdropClose(MON);
        return 0;
    }

    if (const auto MAGIC = hyprdropMagicOpenOn(MON)) {
        dbg("gesture down: closing " + HYPRDROP_MAGIC_NAME + ", back to the workspace under it");
        if (const auto RES = Config::Actions::toggleSpecial(MAGIC); !RES)
            dbg("gesture down: closing " + HYPRDROP_MAGIC_NAME + " failed: " + RES.error().message);
        return 0;
    }

    dbg("gesture down: nothing to do");
    return 0;
}

static int                                   g_hyprdropLiveFrames = 0; // debug: frames drawn since opening

static std::chrono::steady_clock::time_point g_hyprdropLiveSince;

// Before each frame: if we just opened, capture.
static void hyprdropPreRender(PHLMONITOR mon) {
    if (!g_hyprdropOpen || !g_hyprdropNeedCapture || !mon || mon != hyprdropMonitor())
        return;

    g_hyprdropNeedCapture = false;
    hyprdropCapture(mon);
    g_hyprdropLiveFrames = 0;
    g_hyprdropLiveSince  = std::chrono::steady_clock::now();
}

// Before each frame (after the first capture): every workspace of the strip, and the
// active one, is captured again. The next frame is requested once this one is done.
static void hyprdropPreRenderLive(PHLMONITOR mon) {
    if (!hyprdropVisible() || g_hyprdropNeedCapture || !mon || mon != hyprdropMonitor())
        return;

    if (g_hyprdropClosing && hyprdropOpenProgress() <= 0.0) {
        g_hyprdropClosing = false;
        Desktop::globalWindowController()->updateSuspendedStates(); // windows woken up by the capture
        const double MS = hyprdropElapsedMs(g_hyprdropLiveSince);
        dbg(std::format("close animation done: {} frames in {:.0f} ms ({:.0f} fps)", g_hyprdropLiveFrames, MS, g_hyprdropLiveFrames * 1000.0 / std::max(MS, 1.0)));
        g_pHyprRenderer->damageMonitor(mon);
        return;
    }

    ++g_hyprdropLiveFrames;
    g_hyprdropLive = true;
    hyprdropRecaptureWorkspace(mon, g_hyprdropActiveID);
    for (const auto ID : g_hyprdropTiles) {
        if (ID != g_hyprdropActiveID)
            hyprdropRecaptureWorkspace(mon, ID);
    }
    g_hyprdropLive = false;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // Each load starts a fresh log, so a test only contains its own lines.
    std::error_code ec;
    std::filesystem::remove(hyprdropLogPath(), ec);

    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash()) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprdrop] version mismatch, recompile the plugin",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprdrop] version mismatch");
    }

    static auto P1 = Event::bus()->m_events.render.pre.listen([](PHLMONITOR m) {
        hyprdropPreRenderLive(m); // before the first capture: it sees g_hyprdropNeedCapture
        hyprdropPreRender(m);
    });

    static auto P2 = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        // Drawn only on the overview's monitor: this hook runs for every monitor rendered.
        if (stage == RENDER_POST_WINDOWS) {
            if (const auto MON = hyprdropMonitor(); MON && g_pHyprRenderer->m_renderData.pMonitor == MON)
                hyprdropDraw(MON);
        }
        // Live windows and animations: every finished frame asks for the next one. Damage
        // added before the frame (render.pre) is consumed by it and schedules nothing.
        else if (stage == RENDER_POST && hyprdropVisible() && !g_hyprdropCapturing) {
            if (const auto MON = hyprdropMonitor())
                g_pHyprRenderer->damageMonitor(MON);
        }
    });

    // Mouse, touchscreen (a finger = the left button) and stylus tip (= the left button).
    static auto P3  = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo& info) { hyprdropOnMove(info); });
    static auto P4  = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { hyprdropOnButton(e, info); });
    static auto P6  = Event::bus()->m_events.input.touch.down.listen([](ITouch::SDownEvent e, Event::SCallbackInfo& info) { hyprdropOnTouchDown(e, info); });
    static auto P7  = Event::bus()->m_events.input.touch.up.listen([](ITouch::SUpEvent e, Event::SCallbackInfo& info) { hyprdropOnTouchUp(e, info); });
    static auto P8  = Event::bus()->m_events.input.touch.motion.listen([](ITouch::SMotionEvent e, Event::SCallbackInfo& info) { hyprdropOnTouchMotion(e, info); });
    static auto P9  = Event::bus()->m_events.input.touch.cancel.listen([](ITouch::SCancelEvent e, Event::SCallbackInfo& info) { hyprdropOnTouchCancel(e, info); });
    static auto P10 = Event::bus()->m_events.input.tablet.tip.listen([](CTablet::STipEvent e, Event::SCallbackInfo& info) { hyprdropOnTabletTip(e, info); });

    static auto P15 = Event::bus()->m_events.gesture.swipe.begin.listen([](IPointer::SSwipeBeginEvent e, Event::SCallbackInfo& info) { hyprdropOnSwipeBegin(e, info); });
    static auto P16 = Event::bus()->m_events.gesture.swipe.update.listen([](IPointer::SSwipeUpdateEvent e, Event::SCallbackInfo& info) { hyprdropOnSwipeUpdate(e, info); });
    static auto P17 = Event::bus()->m_events.gesture.swipe.end.listen([](IPointer::SSwipeEndEvent e, Event::SCallbackInfo& info) { hyprdropOnSwipeEnd(e, info); });

    static auto P14 = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) { hyprdropOnKey(e, info); });

    static auto P5 = Event::bus()->m_events.input.mouse.axis.listen([](IPointer::SAxisEvent, Event::SCallbackInfo& info) { hyprdropOnAxis(info); });

    static auto P18 = Event::bus()->m_events.gesture.pinch.begin.listen([](IPointer::SPinchBeginEvent, Event::SCallbackInfo& info) { hyprdropOnPinch(true, false, info); });
    static auto P19 = Event::bus()->m_events.gesture.pinch.update.listen([](IPointer::SPinchUpdateEvent, Event::SCallbackInfo& info) { hyprdropOnPinch(false, false, info); });
    static auto P20 = Event::bus()->m_events.gesture.pinch.end.listen([](IPointer::SPinchEndEvent, Event::SCallbackInfo& info) { hyprdropOnPinch(false, true, info); });

    // Created disarmed; armed by hyprdropArmHover.
    g_hyprdropHoverTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { hyprdropOnHoverTimer(); }, nullptr);
    g_pEventLoopManager->addTimer(g_hyprdropHoverTimer);

    // Live preview: created disarmed, armed while a drag rests in the top view.
    g_hyprdropPreviewTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { hyprdropOnPreviewTimer(); }, nullptr);
    g_pEventLoopManager->addTimer(g_hyprdropPreviewTimer);

    // Double click: created disarmed, armed by the first click on a window.
    g_hyprdropClickTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { hyprdropOnClickTimer(); }, nullptr);
    g_pEventLoopManager->addTimer(g_hyprdropClickTimer);

    g_hyprdropZqsd = makeShared<Config::Values::CBoolValue>("plugin:hyprdrop:zqsd", "Z Q S D (W A S D positions) also navigate in the overview", false);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_hyprdropZqsd);
    g_hyprdropIconTheme = makeShared<Config::Values::CStringValue>("plugin:hyprdrop:icon_theme", "icon theme for app icons, empty = the desktop's", "");
    HyprlandAPI::addConfigValueV2(PHANDLE, g_hyprdropIconTheme);
    g_hyprdropCloseBind = makeShared<Config::Values::CStringValue>("plugin:hyprdrop:close_bind", "MODS + KEY closing the window under the pointer", "");
    HyprlandAPI::addConfigValueV2(PHANDLE, g_hyprdropCloseBind);
    g_hyprdropDebug = makeShared<Config::Values::CBoolValue>("plugin:hyprdrop:debug", "write a debug log to $XDG_RUNTIME_DIR/hyprdrop.log", false);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_hyprdropDebug);

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprdrop", "toggle_hidden", luaToggle);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprdrop", "toggle_current", luaToggleCurrent);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprdrop", "drag", luaDrag);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprdrop", "gesture_up", luaGestureUp);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprdrop", "gesture_down", luaGestureDown);

    return {"hyprdrop", "Workspace overview with drag and drop", "lauwr", "0.6"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // GPU resources are released here, with the GL context current, not at unload time.
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    g_hyprdropLabels.clear();
    g_hyprdropIcons.clear();
    g_hyprdropCaptures.clear();
    g_hyprdropWallpaperFB.reset();

    // Don't leave the cursor shape forced if unloaded while open.
    if (g_hyprdropOpen)
        hyprdropSetCursorOverride(false);
    if (hyprdropVisible())
        Desktop::globalWindowController()->updateSuspendedStates();

    // Otherwise the event loop keeps a callback pointing into the unloaded .so.
    if (g_hyprdropHoverTimer) {
        g_pEventLoopManager->removeTimer(g_hyprdropHoverTimer);
        g_hyprdropHoverTimer.reset();
    }
    if (g_hyprdropClickTimer) {
        g_pEventLoopManager->removeTimer(g_hyprdropClickTimer);
        g_hyprdropClickTimer.reset();
    }
    if (g_hyprdropPreviewTimer) {
        g_pEventLoopManager->removeTimer(g_hyprdropPreviewTimer);
        g_hyprdropPreviewTimer.reset();
    }

}

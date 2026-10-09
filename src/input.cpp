#include "hyprdrop.hpp"

static Vector2D hyprdropMouseLocal(PHLMONITOR mon);
static void hyprdropPointerMoved(PHLMONITOR MON, const Vector2D& P);
static PHLWINDOW hyprdropDesktopWindowAt(PHLMONITOR mon, const Vector2D& globalPos);
static Vector2D hyprdropTouchLocal(PHLMONITOR mon, const Vector2D& norm);
static bool hyprdropKeyPassesThrough(uint32_t key);
static int hyprdropNavKey(uint32_t key);
static void hyprdropNavigate(int nav);
static void hyprdropSwipeStep(int dir);

// Stylus tip on the screen while the overview is open (its press was swallowed).
static bool g_hyprdropTipDown = false;

// Live preview: while a drag rests HYPRDROP_PREVIEW_DELAY on a spot of the top view, the
// window is really placed there, so the top view shows the final layout. Where it was
// before the first preview placement is kept, to put it back if the drag is cancelled
// (approximately for tiled windows). The ghost still follows the cursor.
static constexpr auto HYPRDROP_PREVIEW_DELAY = std::chrono::milliseconds(70);
static Vector2D       g_hyprdropPreviewAnchor; // pointer position when the delay started

// The mouse was just on special:magic's tile or the trash, on the second row: going back up
// to the top view crosses the numbered row, and the tile crossed must not replace what is
// shown. The next tile hovered by the mouse is therefore not shown (once; the pointer
// reaching the top view clears it too). Touch is not concerned: a tap shows its tile.
static bool g_hyprdropSkipNextHover = false;

// A window was just dropped on the second row (a special tile or the trash), or the overview
// just opened (the mouse may be resting where the strip appears): the next numbered tile
// hovered is not shown either, until the pointer reaches the top view.
void hyprdropSkipNextHover() {
    g_hyprdropSkipNextHover = true;
}

// The key whose bind opened the overview (toggle_current() / toggle_hidden() from a key
// bind), -1 = none (gesture, drag...). While open, pressing that key again is let through
// to Hyprland, so the same bind closes it. A bind runs right after our listener let its key
// through: a key pressed within HYPRDROP_OPEN_KEY_WINDOW before opening is taken as its key.
// ponytail: time-based guess, a bind reached otherwise just after a key press could be misread
static constexpr auto                        HYPRDROP_OPEN_KEY_WINDOW = std::chrono::milliseconds(100);
static int64_t                               g_hyprdropLastKey        = -1;
static std::chrono::steady_clock::time_point g_hyprdropLastKeyTime;
static int64_t                               g_hyprdropOpenKey        = -1;

void hyprdropRememberOpenKey() {
    const bool RECENT = std::chrono::steady_clock::now() - g_hyprdropLastKeyTime < HYPRDROP_OPEN_KEY_WINDOW;
    g_hyprdropOpenKey = RECENT ? g_hyprdropLastKey : -1;
    if (g_hyprdropOpenKey >= 0)
        dbg("key: opened by key " + std::to_string(g_hyprdropOpenKey) + ", pressing it again goes to its bind");
}

// Keys whose press the overview swallowed: their release is swallowed too, so Hyprland
// never sees half a key (a stuck modifier...). Releases of keys pressed before opening go through.
static std::set<uint32_t> g_hyprdropBlockedKeys;

// ---------------------------------------------------------------- mouse

// Cursor position relative to the monitor (logical coordinates, same as the layout).
static Vector2D hyprdropMouseLocal(PHLMONITOR mon) {
    return g_pInputManager->getMouseCoordsInternal() - mon->m_position;
}

// While the overview is open the cursor shape is forced to the default arrow, like
// hyprexpo does. Otherwise it keeps the shape of the window that was under it, and
// that window may have hidden it (terminals hide the pointer while typing, video
// players...): mouse events are swallowed, so no window ever gets to restore it.
void hyprdropSetCursorOverride(bool on) {
    if (on)
        Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
    else
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
    dbg(std::string("cursor: override ") + (on ? "set to left_ptr" : "removed"));
}

// The pointer (mouse or finger) moved to `P`: hover, dwell delay and ghost.
static void hyprdropPointerMoved(PHLMONITOR MON, const Vector2D& P) {
    g_hyprdropPointer = P;
    if (g_hyprdropDragWin && !g_hyprdropDragMoved && P.distance(g_hyprdropPressPos) >= HYPRDROP_CLICK_SLOP) {
        g_hyprdropDragMoved = true;
        dbg("drag: moved past the click slop, dragging");
    }

    const auto     L     = hyprdropLayout(MON->m_size);
    const int      hover = hyprdropTileAt(L, P);

    if (const bool TRASH = !L.trash.empty() && L.trash.containsPoint(P); TRASH != g_hyprdropTrashHover) {
        g_hyprdropTrashHover = TRASH;
        g_pHyprRenderer->damageMonitor(MON);
    }

    const bool MOUSE  = g_hyprdropTouchID < 0;
    const bool SECOND = g_hyprdropTrashHover || (hover >= 0 && hyprdropIsSpecialID(g_hyprdropTiles[hover]));
    if (MOUSE && SECOND)
        g_hyprdropSkipNextHover = true;
    else if (L.top.containsPoint(P) && hyprdropOpenProgress() >= 1.0) // while opening, the top view still covers everything
        g_hyprdropSkipNextHover = false;

    if (hover != g_hyprdropHover) {
        g_hyprdropHover = hover;
        dbg("hover: tile " + std::to_string(hover));
        g_pHyprRenderer->damageMonitor(MON);

        // Without a drag, hovering a tile shows it in the top view, like a click.
        if (!hyprdropDragging() && hover >= 0) {
            if (MOUSE && g_hyprdropSkipNextHover && !hyprdropIsSpecialID(g_hyprdropTiles[hover])) {
                g_hyprdropSkipNextHover = false;
                dbg("hover: tile " + std::to_string(hover) + " crossed coming from the second row, not shown");
            } else
                hyprdropShowOnTop(hover, "hover");
        }

        if (hyprdropDragging()) {
            g_hyprdropHoverLocked = false;
            if (hover >= 0) {
                dbg("hover: delay started on tile " + std::to_string(hover));
                hyprdropArmHover(P);
            } else if (g_hyprdropHoverTimer->armed()) {
                dbg("hover: tile left before the delay, cancelled");
                hyprdropDisarmHover();
            }
        }
    } else if (hyprdropDragging() && hover >= 0 && !g_hyprdropHoverLocked && P.distance(g_hyprdropHoverAnchor) > HYPRDROP_HOVER_SLOP) {
        // Still on the same tile but the cursor moved: the delay restarts from zero.
        hyprdropArmHover(P);
    }

    // Live preview: (re)start the delay when the pointer rests somewhere new in the top view.
    if (hyprdropDragging() && g_hyprdropPreviewTimer) {
        if (L.top.containsPoint(P)) {
            if (!g_hyprdropPreviewTimer->armed() || P.distance(g_hyprdropPreviewAnchor) > HYPRDROP_HOVER_SLOP) {
                g_hyprdropPreviewAnchor = P;
                g_hyprdropPreviewTimer->updateTimeout(HYPRDROP_PREVIEW_DELAY);
                g_pEventLoopManager->scheduleRecalc();
            }
        } else if (g_hyprdropPreviewTimer->armed()) {
            g_hyprdropPreviewTimer->updateTimeout(std::nullopt);
            g_pEventLoopManager->scheduleRecalc();
        }
    }

    // The ghost follows the cursor: redraw on every move.
    if (hyprdropDragging())
        g_pHyprRenderer->damageMonitor(MON);
}

// While the overview is open, mouse events are swallowed so the real windows
// don't react (same idea as hyprexpo: info.cancelled = true).
void hyprdropOnMove(Event::SCallbackInfo& info) {
    if (!g_hyprdropOpen)
        return;

    info.cancelled = true;

    // A finger drives the overview: the mouse move Hyprland emits for it must not
    // pull the pointer back to the (unmoved) cursor. Same for our own warps.
    if (g_hyprdropTouchID >= 0 || g_hyprdropWarping)
        return;

    const auto MON = hyprdropMonitor();
    if (!MON)
        return;

    hyprdropPointerMoved(MON, hyprdropMouseLocal(MON));
}

// Window of the real desktop under `globalPos`: the open special workspace first, then
// the active one; floating windows first, top of the stack first.
static PHLWINDOW hyprdropDesktopWindowAt(PHLMONITOR mon, const Vector2D& globalPos) {
    const auto& WINDOWS = Desktop::windowState()->windows();
    for (const auto& WS : {mon->m_activeSpecialWorkspace, mon->m_activeWorkspace}) {
        if (!WS)
            continue;
        for (bool floating : {true, false}) {
            for (auto it = WINDOWS.rbegin(); it != WINDOWS.rend(); ++it) {
                const auto& w = *it;
                if (w && w->m_isMapped && !w->isHidden() && w->m_isFloating == floating && (w->m_workspace == WS || (w->m_pinned && !WS->m_isSpecialWorkspace)) &&
                    w->getWindowMainSurfaceBox().containsPoint(globalPos))
                    return w;
            }
        }
    }
    return nullptr;
}

// Drag mode, started by hl.plugin.hyprdrop.drag() (bound by the user, e.g. to Super +
// left button): opens the overview (D = A) with the window under the cursor already
// dragged. The button release then comes through the mouse button event as usual.
// When drag() is called from a mouse bind, the press that triggered it may reach our
// button listener just after: presses within this delay of drag() are ignored.
static constexpr auto                        HYPRDROP_DRAG_PRESS_GRACE = std::chrono::milliseconds(100);

static std::chrono::steady_clock::time_point g_hyprdropDragStartTime;

void hyprdropStartDrag() {
    if (g_hyprdropOpen) {
        dbg("drag(): overview already open, ignored");
        return;
    }

    const auto MON = hyprdropMonitor();
    if (!MON)
        return;

    const auto WIN = hyprdropDesktopWindowAt(MON, g_pInputManager->getMouseCoordsInternal());
    if (!WIN) {
        dbg("drag(): no window under the cursor, ignored");
        return;
    }

    dbg(std::format("super drag: start on '{}' (workspace {})", WIN->m_title, WIN->m_workspace ? WIN->m_workspace->m_id : WORKSPACE_INVALID));
    hyprdropToggle(HYPRDROP_OPEN_CURRENT);
    g_hyprdropSuperDrag     = true;
    g_hyprdropDragWin       = WIN;
    g_hyprdropDragMoved     = true; // drag mode: no click to wait for
    g_hyprdropDragStartTime = std::chrono::steady_clock::now();
    hyprdropResetPreview();
    g_hyprdropPointer  = hyprdropMouseLocal(MON);
    g_hyprdropPressPos = g_hyprdropPointer;
}

void hyprdropOnButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
    if (!g_hyprdropOpen)
        return;

    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED && g_hyprdropSuperDrag && g_hyprdropDragWin &&
        std::chrono::steady_clock::now() - g_hyprdropDragStartTime < HYPRDROP_DRAG_PRESS_GRACE) {
        info.cancelled = true;
        dbg("button: press that triggered drag(), ignored");
        return;
    }

    info.cancelled = true;

    const bool PRESSED = e.state == WL_POINTER_BUTTON_STATE_PRESSED;
    dbg(std::format("button {} {}, hovered tile = {}", e.button, PRESSED ? "press" : "release", g_hyprdropHover));

    if (e.button != BTN_LEFT)
        return;

    const auto MON = hyprdropMonitor();
    if (!MON)
        return;

    if (g_hyprdropTouchID >= 0) {
        dbg("button: a finger is down on the touchscreen, ignored");
        return;
    }

    g_hyprdropPointer = hyprdropMouseLocal(MON);
    if (PRESSED) {
        g_hyprdropPressIsMouse = true;
        hyprdropOnPress(MON);
    }
    else
        hyprdropOnRelease(MON);
}

// Scroll (wheel, two-finger touchpad scroll): swallowed while the overview is open, so the
// windows under it don't scroll.
void hyprdropOnAxis(Event::SCallbackInfo& info) {
    if (g_hyprdropOpen)
        info.cancelled = true;
}

// Touchpad pinch: swallowed while the overview is open (browsers zoom on it). A pinch whose
// begin was swallowed has its updates and end swallowed too, even after closing.
static bool g_hyprdropPinchSwallowed = false;

void hyprdropOnPinch(bool begin, bool end, Event::SCallbackInfo& info) {
    if (begin)
        g_hyprdropPinchSwallowed = g_hyprdropOpen;
    if (g_hyprdropPinchSwallowed)
        info.cancelled = true;
    if (end)
        g_hyprdropPinchSwallowed = false;
}

// ---------------------------------------------------------------- touchscreen

// Touch positions come normalized (0..1) over the touchscreen's monitor; we assume it is
// the focused one. A finger on the screen = the left button: down = press, motion =
// move, up = release. Events are swallowed while the overview is open.
static Vector2D hyprdropTouchLocal(PHLMONITOR mon, const Vector2D& norm) {
    return norm * mon->m_size;
}

void hyprdropOnTouchDown(const ITouch::SDownEvent& e, Event::SCallbackInfo& info) {
    if (!g_hyprdropOpen)
        return;

    info.cancelled = true;

    if (g_hyprdropTouchID >= 0) {
        dbg(std::format("touch: finger {} down while finger {} drives the overview, ignored", e.touchID, g_hyprdropTouchID));
        return;
    }

    const auto MON = hyprdropMonitor();
    if (!MON)
        return;

    g_hyprdropTouchID = e.touchID;
    const Vector2D P  = hyprdropTouchLocal(MON, e.pos);
    dbg(std::format("touch: finger {} down at ({:.0f},{:.0f}) -> press", e.touchID, P.x, P.y));

    g_hyprdropPressIsMouse = false;
    hyprdropPointerMoved(MON, P); // hover first, so the press sees the tile under the finger
    hyprdropOnPress(MON);
}

void hyprdropOnTouchMotion(const ITouch::SMotionEvent& e, Event::SCallbackInfo& info) {
    if (!g_hyprdropOpen)
        return;

    info.cancelled = true;

    if (e.touchID != g_hyprdropTouchID)
        return;

    if (const auto MON = hyprdropMonitor())
        hyprdropPointerMoved(MON, hyprdropTouchLocal(MON, e.pos));
}

void hyprdropOnTouchUp(const ITouch::SUpEvent& e, Event::SCallbackInfo& info) {
    // Checked before `open`: the down was swallowed, so its up must be too, even if the
    // overview was closed meanwhile.
    if (e.touchID != g_hyprdropTouchID) {
        if (g_hyprdropOpen)
            info.cancelled = true;
        return;
    }

    info.cancelled    = true;
    g_hyprdropTouchID = -1;

    if (!g_hyprdropOpen) {
        dbg(std::format("touch: finger {} up after the overview closed", e.touchID));
        return;
    }

    dbg(std::format("touch: finger {} up at ({:.0f},{:.0f}) -> release", e.touchID, g_hyprdropPointer.x, g_hyprdropPointer.y));
    if (const auto MON = hyprdropMonitor())
        hyprdropOnRelease(MON);
}

void hyprdropOnTouchCancel(const ITouch::SCancelEvent& e, Event::SCallbackInfo& info) {
    if (e.touchID != g_hyprdropTouchID)
        return;

    info.cancelled    = true;
    g_hyprdropTouchID = -1;
    dbg(std::format("touch: finger {} cancelled, drag cancelled", e.touchID));
    hyprdropCancelDrag();
}

// Keys that still work while the overview is open: the laptop Fn-layer keys (volume,
// brightness, media, keyboard backlight). Every other key is swallowed, so no bind runs.
static bool hyprdropKeyPassesThrough(uint32_t key) {
    switch (key) {
        case KEY_MUTE:
        case KEY_VOLUMEDOWN:
        case KEY_VOLUMEUP:
        case KEY_MICMUTE:
        case KEY_BRIGHTNESSDOWN:
        case KEY_BRIGHTNESSUP:
        case KEY_KBDILLUMTOGGLE:
        case KEY_KBDILLUMDOWN:
        case KEY_KBDILLUMUP:
        case KEY_PLAYPAUSE:
        case KEY_PLAYCD:
        case KEY_PAUSECD:
        case KEY_STOPCD:
        case KEY_NEXTSONG:
        case KEY_PREVIOUSSONG: return true;
        default: return false;
    }
}

// Navigation keys: -1/+1 = previous/next numbered workspace, 2 = down to special:magic,
// -2 = back up to the numbered row. 0 = not a navigation key.
static int hyprdropNavKey(uint32_t key) {
    const bool ZQSD = g_hyprdropZqsd && g_hyprdropZqsd->value();
    switch (key) {
        case KEY_LEFT:
        case KEY_KP4: return -1;
        case KEY_RIGHT:
        case KEY_KP6: return +1;
        case KEY_UP:
        case KEY_KP8: return -2;
        case KEY_DOWN:
        case KEY_KP2: return 2;
        case KEY_A: return ZQSD ? -1 : 0;
        case KEY_D: return ZQSD ? +1 : 0;
        case KEY_W: return ZQSD ? -2 : 0;
        case KEY_S: return ZQSD ? 2 : 0;
        default: return 0;
    }
}

static void hyprdropNavigate(int nav) {
    if (nav == -1 || nav == 1) {
        hyprdropSwipeStep(nav);
        return;
    }
    if (nav == 2) {
        const int SPECIALS = hyprdropSpecialTiles();
        if (SPECIALS == 0 || hyprdropIsSpecialID(g_hyprdropTopID))
            return;
        g_hyprdropBeforeMagicID = g_hyprdropTopID;
        hyprdropShowOnTop((int)g_hyprdropTiles.size() - SPECIALS, "key down"); // the first special tile
        return;
    }
    if (!hyprdropIsSpecialID(g_hyprdropTopID))
        return;
    const WORKSPACEID BACK = g_hyprdropBeforeMagicID != WORKSPACE_INVALID ? g_hyprdropBeforeMagicID : g_hyprdropActiveID;
    if (const auto IT = std::ranges::find(g_hyprdropTiles, BACK); IT != g_hyprdropTiles.end())
        hyprdropShowOnTop((int)(IT - g_hyprdropTiles.begin()), "key up");
}

// plugin:hyprdrop:close_bind ("SUPER + C"...) matches this key press: same key (by keysym
// name, any case) and same modifiers (Caps Lock and Num Lock ignored).
static bool hyprdropIsCloseBind(uint32_t keycode) {
    const std::string BIND = g_hyprdropCloseBind ? std::string{g_hyprdropCloseBind->value()} : "";
    const auto        KB   = g_pSeatManager->m_keyboard.lock();
    if (BIND.empty() || !KB || !KB->m_xkbState)
        return false;

    const auto  PLUS = BIND.rfind('+');
    std::string key  = PLUS == std::string::npos ? BIND : BIND.substr(PLUS + 1);
    std::erase_if(key, ::isspace);
    const uint32_t WANTMODS = PLUS == std::string::npos ? 0 : g_pKeybindManager->stringToModMask(BIND.substr(0, PLUS));

    char           name[64] = {};
    xkb_keysym_get_name(xkb_state_key_get_one_sym(KB->m_xkbState, keycode + 8), name, sizeof(name));
    const uint32_t IGNORED = HL_MODIFIER_CAPS | HL_MODIFIER_MOD2;
    return strcasecmp(name, key.c_str()) == 0 && (KB->getModifiers() & ~IGNORED) == (WANTMODS & ~IGNORED);
}

// While the overview is open: Escape closes, Enter / keypad Enter / Space go to the
// workspace shown in the top view and close, arrows (keypad 4 8 6 2, optionally Z Q S D)
// change the workspace shown, Delete / BackSpace / close_bind close the window under the
// pointer, Fn-layer keys pass, every other key is swallowed.
void hyprdropOnKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
    const bool PRESSED = e.state == WL_KEYBOARD_KEY_STATE_PRESSED;

    if (!PRESSED) {
        if (g_hyprdropBlockedKeys.erase(e.keycode))
            info.cancelled = true;
        return;
    }

    g_hyprdropLastKey     = e.keycode;
    g_hyprdropLastKeyTime = std::chrono::steady_clock::now();

    if (!g_hyprdropOpen || hyprdropKeyPassesThrough(e.keycode))
        return;

    if ((int64_t)e.keycode == g_hyprdropOpenKey) {
        dbg("key: the key that opened the overview, let through to its bind");
        return;
    }

    info.cancelled = true;
    g_hyprdropBlockedKeys.insert(e.keycode);

    const auto MON = hyprdropMonitor();
    if (MON && (e.keycode == KEY_DELETE || e.keycode == KEY_BACKSPACE || hyprdropIsCloseBind(e.keycode))) {
        if (!hyprdropCloseHovered(MON))
            dbg("key: close key, but no window under the pointer");
    } else if (e.keycode == KEY_ESC) {
        dbg("key: Escape, closing");
        hyprdropClose(MON);
    } else if (e.keycode == KEY_ENTER || e.keycode == KEY_KPENTER || e.keycode == KEY_SPACE) {
        const WORKSPACEID ID = hyprdropShownID();
        dbg("key: Enter/Space, going to workspace " + std::to_string(ID));
        if (MON)
            hyprdropGoToWorkspace(MON, ID);
    } else if (const int NAV = hyprdropNavKey(e.keycode)) {
        dbg("key: " + std::to_string(e.keycode) + ", navigation " + std::to_string(NAV));
        hyprdropNavigate(NAV);
    } else
        dbg("key: " + std::to_string(e.keycode) + " swallowed");
}

// ---------------------------------------------------------------- stylus

// The stylus moves the cursor itself (Hyprland emits mouse moves for it), so only its
// tip needs handling: tip down = left button press, tip up = release.
void hyprdropOnTabletTip(const CTablet::STipEvent& e, Event::SCallbackInfo& info) {
    // Checked before `open`: the down was swallowed, so its up must be too, even if the
    // overview was closed meanwhile.
    if (!e.in && g_hyprdropTipDown) {
        info.cancelled   = true;
        g_hyprdropTipDown = false;
        if (!g_hyprdropOpen) {
            dbg("stylus: tip up after the overview closed");
            return;
        }
    } else if (!g_hyprdropOpen)
        return;
    else
        info.cancelled = true;

    if (g_hyprdropTouchID >= 0) {
        dbg("stylus: a finger is down on the touchscreen, ignored");
        return;
    }

    const auto MON = hyprdropMonitor();
    if (!MON)
        return;

    g_hyprdropPointer = hyprdropMouseLocal(MON);
    dbg(std::format("stylus: tip {} at ({:.0f},{:.0f}) -> {}", e.in ? "down" : "up", g_hyprdropPointer.x, g_hyprdropPointer.y, e.in ? "press" : "release"));

    if (e.in) {
        g_hyprdropTipDown      = true;
        g_hyprdropPressIsMouse = false;
        hyprdropOnPress(MON);
    } else
        hyprdropOnRelease(MON);
}

// ---------------------------------------------------------------- gestures

// Horizontal touchpad swipe while the overview is open: shows the previous / next numbered
// workspace in the top view, without going there (one step per swipe, like Hyprland's
// workspace swipe: fingers to the left = next), as soon as the swipe is long enough. Vertical swipes are left alone, for
// gesture_up() / gesture_down(). Once a swipe is seen to be horizontal, its updates are
// swallowed so Hyprland's own workspace swipe doesn't run; its end is let through so
// Hyprland's gesture state is closed.
static constexpr double HYPRDROP_SWIPE_DECIDE    = 30.0;  // accumulated delta needed to tell the direction

static constexpr double HYPRDROP_SWIPE_THRESHOLD = 150.0; // accumulated horizontal delta for one step

static bool             g_hyprdropSwipeActive     = false;

static bool             g_hyprdropSwipeHorizontal = false;

static Vector2D         g_hyprdropSwipeDelta;

static void hyprdropSwipeStep(int dir) {
    // Numbered tiles only (special:magic is on its own row).
    std::vector<int> numbered;
    for (int i = 0; i < (int)g_hyprdropTiles.size(); ++i) {
        if (!hyprdropIsSpecialID(g_hyprdropTiles[i]))
            numbered.push_back(i);
    }
    if (numbered.empty())
        return;

    int cur = -1;
    for (int k = 0; k < (int)numbered.size(); ++k) {
        if (g_hyprdropTiles[numbered[k]] == g_hyprdropTopID)
            cur = k;
    }

    int next = cur < 0 ? (dir > 0 ? 0 : (int)numbered.size() - 1) : cur + dir;
    if (next < 0 || next >= (int)numbered.size()) {
        dbg("swipe: already at the " + std::string(dir > 0 ? "last" : "first") + " workspace");
        return;
    }
    hyprdropShowOnTop(numbered[next], "swipe");
}

void hyprdropOnSwipeBegin(const IPointer::SSwipeBeginEvent& e, Event::SCallbackInfo&) {
    g_hyprdropSwipeActive     = g_hyprdropOpen;
    g_hyprdropSwipeHorizontal = false;
    g_hyprdropSwipeDelta      = {};
    g_hyprdropSwipeStepped    = false;
    if (g_hyprdropSwipeActive)
        dbg(std::format("swipe: begin ({} fingers)", e.fingers));
}

void hyprdropOnSwipeUpdate(const IPointer::SSwipeUpdateEvent& e, Event::SCallbackInfo& info) {
    if (!g_hyprdropSwipeActive || !g_hyprdropOpen)
        return;

    g_hyprdropSwipeDelta = g_hyprdropSwipeDelta + e.delta;

    if (!g_hyprdropSwipeHorizontal) {
        // Undecided updates are swallowed too: Hyprland's own workspace swipe would start
        // moving the active workspace, and the live capture shows it shaking. A vertical
        // swipe only loses these first few pixels.
        if (std::abs(g_hyprdropSwipeDelta.x) < HYPRDROP_SWIPE_DECIDE && std::abs(g_hyprdropSwipeDelta.y) < HYPRDROP_SWIPE_DECIDE) {
            info.cancelled = true;
            return;
        }
        if (std::abs(g_hyprdropSwipeDelta.y) >= std::abs(g_hyprdropSwipeDelta.x)) {
            dbg("swipe: vertical, left to Hyprland");
            g_hyprdropSwipeActive = false;
            return;
        }
        g_hyprdropSwipeHorizontal = true;
        dbg("swipe: horizontal, handled by hyprdrop");
    }

    info.cancelled = true;

    // One step per swipe, as soon as it is long enough (not on release).
    if (!g_hyprdropSwipeStepped && std::abs(g_hyprdropSwipeDelta.x) >= HYPRDROP_SWIPE_THRESHOLD) {
        g_hyprdropSwipeStepped = true;
        hyprdropSwipeStep(g_hyprdropSwipeDelta.x < 0 ? +1 : -1);
    }
}

void hyprdropOnSwipeEnd(const IPointer::SSwipeEndEvent& e, Event::SCallbackInfo&) {
    const bool WAS = g_hyprdropSwipeActive && g_hyprdropSwipeHorizontal;
    g_hyprdropSwipeActive     = false;
    g_hyprdropSwipeHorizontal = false;
    if (!WAS || !g_hyprdropOpen)
        return;

    dbg(std::format("swipe: end, dx = {:.0f}{}{}", g_hyprdropSwipeDelta.x, e.cancelled ? " (cancelled)" : "", g_hyprdropSwipeStepped ? ", workspace changed" : ""));
}

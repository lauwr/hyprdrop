#include "hyprdrop.hpp"

static std::chrono::milliseconds hyprdropDoubleClickDelay();
static void hyprdropGoToWindow(PHLMONITOR mon, PHLWINDOW WIN);
static void hyprdropOnWindowClick(PHLMONITOR mon, PHLWINDOW WIN);
static void hyprdropPlaceAt(PHLMONITOR mon, PHLWINDOW WIN, const Vector2D& DROP, const Vector2D& CORNER);
static void hyprdropPlaceInTop(PHLMONITOR mon, PHLWINDOW WIN, const Vector2D& P);
static void hyprdropMoveAndStay(PHLMONITOR mon, PHLWINDOW WIN, WORKSPACEID id);
static bool hyprdropMoveWindowSilently(PHLMONITOR mon, PHLWINDOW WIN, WORKSPACEID targetID);
static void hyprdropMoveWindowTo(PHLMONITOR mon, PHLWINDOW WIN, WORKSPACEID targetID);

// Click on a window of the top view: a double click (or double tap) goes to the window
// and closes the overview. A single click only acts once HYPRDROP_DOUBLE_CLICK_DELAY has
// passed without a second one: it brings the window back to A (when D != A).
static constexpr auto      HYPRDROP_DOUBLE_CLICK_DELAY = std::chrono::milliseconds(200);

// A finger or the stylus is slower: double taps get more time.
static constexpr auto      HYPRDROP_DOUBLE_TAP_DELAY = std::chrono::milliseconds(300);

static std::chrono::milliseconds hyprdropDoubleClickDelay() {
    return g_hyprdropPressIsMouse ? HYPRDROP_DOUBLE_CLICK_DELAY : HYPRDROP_DOUBLE_TAP_DELAY;
}

static std::chrono::steady_clock::time_point g_hyprdropTileTapTime; // see g_hyprdropTileTapID

// Live preview: the drag already placed the window once; where it was before (workspace
// here, box in g_hyprdropPreviewFromBox), to put it back if the drag is cancelled.
static bool        g_hyprdropPreviewDone   = false;
static WORKSPACEID g_hyprdropPreviewFromWS = WORKSPACE_INVALID;

// Pressed on the background (neither the top view nor a tile): releasing there too
// closes the overview, as Escape does.
static bool g_hyprdropBackgroundPress = false;

// Tile under the pointer when pressed outside the top view, -1 = none. Releasing on the
// same tile is a click on it.
static int g_hyprdropTilePress = -1;

// Pressed on the trash: releasing there too toggles the delete mode.
static bool g_hyprdropTrashPress = false;

// Dwell hover during a drag: how long to rest on a tile.
static constexpr auto       HYPRDROP_HOVER_DELAY = std::chrono::milliseconds(500);

void hyprdropDisarmHover() {
    if (g_hyprdropHoverTimer && g_hyprdropHoverTimer->armed()) {
        g_hyprdropHoverTimer->updateTimeout(std::nullopt);
        g_pEventLoopManager->scheduleRecalc();
    }
}

void hyprdropArmHover(const Vector2D& P) {
    g_hyprdropHoverAnchor = P;
    g_hyprdropHoverTimer->updateTimeout(HYPRDROP_HOVER_DELAY);
    g_pEventLoopManager->scheduleRecalc();
}

// A window is held and the pointer has moved: a real drag.
bool hyprdropDragging() {
    return g_hyprdropDragWin && g_hyprdropDragMoved;
}

void hyprdropCancelDrag() {
    g_hyprdropDragWin.reset();
    g_hyprdropDragMoved = false;
    if (g_hyprdropPreviewTimer && g_hyprdropPreviewTimer->armed()) {
        g_hyprdropPreviewTimer->updateTimeout(std::nullopt);
        g_pEventLoopManager->scheduleRecalc();
    }
    g_hyprdropBackgroundPress = false;
    g_hyprdropTilePress = -1;
    g_hyprdropTrashPress = false;
    g_hyprdropHoverLocked = false;
    hyprdropDisarmHover();
}

// The workspace of tile `TILE` becomes D, shown in the top view. Only existing captures
// are reused, no new capture; an empty tile gives an empty top view. The strip doesn't
// move (D keeps its tile, greyed). `why` is for the log: "dwell" or "click".
void hyprdropShowOnTop(int TILE, const char* why) {
    const WORKSPACEID NEWD = g_hyprdropTiles[TILE];
    if (NEWD == g_hyprdropTopID) {
        dbg(std::format("{}: tile {} is workspace {}, already in the top view", why, TILE, NEWD));
        return;
    }

    dbg(std::format("{}: tile {} -> top view switches from workspace {} to workspace {}{}", why, TILE, g_hyprdropTopID, NEWD,
                    hyprdropCaptureOf(NEWD) ? "" : " (empty)"));
    const auto OLDIT = std::ranges::find(g_hyprdropTiles, g_hyprdropTopID);
    g_hyprdropSlideDir    = OLDIT == g_hyprdropTiles.end() || TILE > (int)(OLDIT - g_hyprdropTiles.begin()) ? 1 : -1;
    g_hyprdropSlideFromID = g_hyprdropTopID;
    g_hyprdropSlideStart  = std::chrono::steady_clock::now();
    g_hyprdropTopID = NEWD;

    if (const auto MON = Desktop::focusState()->monitor())
        g_pHyprRenderer->damageMonitor(MON);
}

// The cursor stayed HYPRDROP_HOVER_DELAY on a tile during a drag: that tile's workspace
// becomes D. The cursor is still on the tile, so it is locked: it must leave the tile
// before the delay can start again.
void hyprdropOnHoverTimer() {
    const int TILE = g_hyprdropHover;
    if (!g_hyprdropOpen || !g_hyprdropDragWin || TILE < 0 || TILE >= (int)g_hyprdropTiles.size()) {
        dbg("dwell: no drag or tile anymore, ignored");
        return;
    }

    hyprdropShowOnTop(TILE, "dwell");
    g_hyprdropDwellID     = g_hyprdropTiles[TILE];
    g_hyprdropHoverLocked = true;
    dbg("dwell: tile " + std::to_string(TILE) + " locked until the cursor leaves it");
}

void hyprdropClose(PHLMONITOR mon) {
    if (!g_hyprdropOpen)
        return;
    // Closed in the middle of a drag (Escape...): undo the preview placements.
    hyprdropRestorePreview(mon, g_hyprdropDragWin.lock());
    g_hyprdropOpen       = false;
    hyprdropStartOpenAnim();
    g_hyprdropSuperDrag  = false;
    hyprdropCancelPendingClick("overview closed");
    hyprdropSetCursorOverride(false);
    g_hyprdropHover      = -1;
    g_hyprdropTrashHover = false;
    g_hyprdropDeleteMode = false;
    hyprdropCancelDrag();
    dbg("overview closed");
    if (mon)
        g_pHyprRenderer->damageMonitor(mon);
}

// Drag mode: the ghost is the window at the top view's scale, centered on the cursor.
void hyprdropSizeSuperDragGhost(PHLMONITOR mon) {
    const auto WIN = g_hyprdropDragWin.lock();
    if (!g_hyprdropSuperDrag || !WIN)
        return;

    const auto     L   = hyprdropLayout(mon->m_size);
    const double   k   = L.top.w / g_hyprdropCaptureUsable.w;
    const CBox     BOX = WIN->getWindowMainSurfaceBox();

    g_hyprdropDragSize   = BOX.size() * k;
    g_hyprdropDragOffset = g_hyprdropDragSize / 2.0; // held by its center, like the ghost
    dbg(std::format("super drag: ghost {:.0f}x{:.0f}", g_hyprdropDragSize.x, g_hyprdropDragSize.y));
    g_pHyprRenderer->damageMonitor(mon);
}

void hyprdropCancelPendingClick(const char* why) {
    if (!g_hyprdropClickWin && !(g_hyprdropClickTimer && g_hyprdropClickTimer->armed()))
        return;
    if (const auto W = g_hyprdropClickWin.lock())
        dbg(std::format("click: pending single click on '{}' dropped ({})", W->m_title, why));
    g_hyprdropClickWin.reset();
    if (g_hyprdropClickTimer && g_hyprdropClickTimer->armed()) {
        g_hyprdropClickTimer->updateTimeout(std::nullopt);
        g_pEventLoopManager->scheduleRecalc();
    }
}

// Goes to workspace `id` (created if needed; special:magic is toggled open) and closes
// the overview.
void hyprdropGoToWorkspace(PHLMONITOR mon, WORKSPACEID id) {
    dbg("going to workspace " + std::to_string(id));
    hyprdropClose(mon);

    Config::Actions::ActionResult res;
    if (id == g_hyprdropMagicID) {
        const auto WS = hyprdropFindWorkspace(id);
        if (!WS) {
            dbg("go: " + HYPRDROP_MAGIC_NAME + " doesn't exist, nothing to open");
            return;
        }
        res = Config::Actions::toggleSpecial(WS);
    } else
        res = Config::Actions::changeWorkspace(std::to_string(id));

    if (!res)
        dbg("go: changing workspace failed: " + res.error().message);
    else
        dbg("go: workspace change ok");
}

// Double click: go to the window (its workspace becomes the active one, special:magic
// gets opened) and close the overview.
static void hyprdropGoToWindow(PHLMONITOR mon, PHLWINDOW WIN) {
    dbg(std::format("go: going to '{}' on workspace {}", WIN->m_title, WIN->m_workspace ? WIN->m_workspace->m_id : WORKSPACE_INVALID));
    hyprdropClose(mon);
    const auto RES = Config::Actions::focus(WIN);
    if (!RES)
        dbg("go: focus failed: " + RES.error().message);
    else
        dbg("go: focus ok");
}

// No second click came in time: single click action.
void hyprdropOnClickTimer() {
    const auto WIN = g_hyprdropClickWin.lock();
    g_hyprdropClickWin.reset();
    const auto MON = Desktop::focusState()->monitor();
    if (!g_hyprdropOpen || !WIN || !MON) {
        dbg("click: single click expired, overview closed or window gone, ignored");
        return;
    }

    if (g_hyprdropTopID == g_hyprdropActiveID) {
        dbg(std::format("click: single click on '{}' while D = A, nothing to bring back", WIN->m_title));
        return;
    }

    dbg(std::format("click: single click on '{}' while D = {} != A, bringing it back to A = {}", WIN->m_title, g_hyprdropTopID, g_hyprdropActiveID));
    hyprdropMoveWindowTo(MON, WIN, g_hyprdropActiveID);
}

// A click (press and release without moving) on a window of the top view.
static void hyprdropOnWindowClick(PHLMONITOR mon, PHLWINDOW WIN) {
    // Delete mode: a single click closes the window at once (no double click), and the
    // overview stays open for the next one.
    if (g_hyprdropDeleteMode) {
        hyprdropCancelPendingClick("delete mode");
        dbg("click: delete mode, closing '" + WIN->m_title + "'");
        if (const auto RES = Config::Actions::closeWindow(WIN); !RES)
            dbg("click: closing the window failed: " + RES.error().message);
        g_pHyprRenderer->damageMonitor(mon);
        return;
    }

    if (g_hyprdropClickWin.lock() == WIN && g_hyprdropClickTimer->armed()) {
        g_hyprdropClickWin.reset();
        g_hyprdropClickTimer->updateTimeout(std::nullopt);
        g_pEventLoopManager->scheduleRecalc();
        hyprdropGoToWindow(mon, WIN);
        return;
    }

    // On A there is nothing to bring back: a single click already goes to the window.
    if (g_hyprdropTopID == g_hyprdropActiveID) {
        hyprdropGoToWindow(mon, WIN);
        return;
    }

    hyprdropCancelPendingClick("click on another window");
    g_hyprdropClickWin = WIN;
    g_hyprdropClickTimer->updateTimeout(hyprdropDoubleClickDelay());
    g_pEventLoopManager->scheduleRecalc();
    dbg(std::format("click: on '{}', waiting {} ms for a double click", WIN->m_title, hyprdropDoubleClickDelay().count()));
}

void hyprdropOnPress(PHLMONITOR mon) {
    const Vector2D P = g_hyprdropPointer;
    const auto     L = hyprdropLayout(mon->m_size);

    if (!L.top.containsPoint(P)) {
        hyprdropCancelPendingClick("press outside the top view");
        if (!L.trash.empty() && L.trash.containsPoint(P)) {
            g_hyprdropTrashPress = true;
            dbg("press: on the trash");
            return;
        }
        g_hyprdropTilePress = hyprdropTileAt(L, P);
        if (g_hyprdropTilePress >= 0)
            dbg("press: on tile " + std::to_string(g_hyprdropTilePress) + " (workspace " + std::to_string(g_hyprdropTiles[g_hyprdropTilePress]) + ")");
        else {
            g_hyprdropBackgroundPress = true;
            dbg("press: on the background");
        }
        return;
    }

    const auto TOPWS = hyprdropFindWorkspace(g_hyprdropTopID);
    if (!TOPWS) {
        dbg("press: workspace " + std::to_string(g_hyprdropTopID) + " doesn't exist, no window to drag");
        return;
    }

    // Hit-test the windows as they are drawn in the top view (captured boxes, gap included).
    const auto HIT = hyprdropCapturedWindowAt(g_hyprdropTopID, L.top, P);
    if (!HIT) {
        hyprdropCancelPendingClick("press on no window");
        dbg(std::format("press: no window at ({:.0f},{:.0f}) in the top view of workspace {}", P.x, P.y, TOPWS->m_id));
        // D isn't shown: if its windows aren't where we think, this is why.
        if (g_hyprdropTopID != g_hyprdropActiveID)
            hyprdropCheckPositions(mon, TOPWS);
        return;
    }

    const auto& [WIN, PLACED] = *HIT;

    if (g_hyprdropClickWin && g_hyprdropClickWin.lock() != WIN)
        hyprdropCancelPendingClick("press on another window");

    g_hyprdropDragWin    = WIN;
    g_hyprdropDwellID    = WORKSPACE_INVALID;
    hyprdropResetPreview();
    g_hyprdropDragSize   = PLACED.win.size();
    g_hyprdropDragOffset = PLACED.win.size() / 2.0; // held by its center, like the ghost
    g_hyprdropPressPos   = P;

    dbg(std::format("press: dragging '{}' (ghost {:.0f}x{:.0f})", WIN->m_title, g_hyprdropDragSize.x, g_hyprdropDragSize.y));
    g_pHyprRenderer->damageMonitor(mon);
}

// Places WIN, on its current workspace, at DROP (global point of the real layout).
// Tiled: Hyprland's own mouse drag is replayed (begin / move / end). Its drop uses the
// real cursor position, so the cursor is warped for the duration to the drop point, then
// put back. Floating: its top-left corner is moved to CORNER.
static void hyprdropPlaceAt(PHLMONITOR mon, PHLWINDOW WIN, const Vector2D& DROP, const Vector2D& CORNER) {
    const auto TARGET = WIN->layoutTarget();
    if (!TARGET) {
        dbg("place: '" + WIN->m_title + "' has no layout target, nothing done");
        return;
    }

    // Hyprland's drop focuses the window, which switches to its workspace when that one
    // isn't shown. Placing must never change where the user is: remembered, then restored.
    const PHLWORKSPACE PREVWS      = mon->m_activeWorkspace;
    const PHLWORKSPACE PREVSPECIAL = mon->m_activeSpecialWorkspace;
    const PHLWINDOW    PREVWIN     = Desktop::focusState()->window();

    const CBox BEFORE = WIN->getWindowMainSurfaceBox();
    dbg(std::format("place: '{}' ({}) on workspace {}, drop at ({:.0f},{:.0f}) global, was ({:.0f},{:.0f} {:.0f}x{:.0f})", WIN->m_title,
                    WIN->m_isFloating ? "floating" : "tiled", WIN->m_workspace ? WIN->m_workspace->m_id : WORKSPACE_INVALID, DROP.x, DROP.y, BEFORE.x, BEFORE.y,
                    BEFORE.w, BEFORE.h));

    if (WIN->m_isFloating) {
        g_layoutManager->setTargetGeom(CBox{CORNER, BEFORE.size()}, TARGET);
        dbg(std::format("place: floating window moved to ({:.0f},{:.0f})", CORNER.x, CORNER.y));
    } else {
        const Vector2D SAVED = g_pInputManager->getMouseCoordsInternal();
        g_hyprdropWarping    = true;
        Pointer::pointerController()->warpTo(DROP, true);

        g_layoutManager->beginDragTarget(TARGET, MBIND_MOVE);
        g_layoutManager->moveMouse(DROP + Vector2D{1, 1}); // a real move, to pass the drag threshold
        g_layoutManager->moveMouse(DROP);
        dbg(std::format("place: native drag, threshold reached = {}, tiled = {}", g_layoutManager->dragController()->dragThresholdReached(),
                        g_layoutManager->dragController()->draggingTiled()));
        g_layoutManager->endDragTarget();

        Pointer::pointerController()->warpTo(SAVED, true);
        g_hyprdropWarping = false;
    }

    if (mon->m_activeSpecialWorkspace != PREVSPECIAL) {
        dbg("place: the drop changed the open special workspace, restoring it");
        if (const auto NOWSPECIAL = mon->m_activeSpecialWorkspace) {
            if (const auto RES = Config::Actions::toggleSpecial(NOWSPECIAL); !RES) // closes it
                dbg("place: closing the special workspace failed: " + RES.error().message);
        }
        if (PREVSPECIAL) {
            if (const auto RES = Config::Actions::toggleSpecial(PREVSPECIAL); !RES)
                dbg("place: reopening the special workspace failed: " + RES.error().message);
        }
    }
    if (PREVWS && mon->m_activeWorkspace != PREVWS) {
        dbg(std::format("place: the drop switched to workspace {}, going back to {}", mon->m_activeWorkspace ? mon->m_activeWorkspace->m_id : WORKSPACE_INVALID,
                        PREVWS->m_id));
        if (const auto RES = Config::Actions::changeWorkspace(PREVWS); !RES)
            dbg("place: going back failed: " + RES.error().message);
    }
    // Give the focus back, unless that window left the workspace the user is on.
    if (PREVWIN && PREVWIN != Desktop::focusState()->window() && PREVWIN->m_workspace == PREVWS) {
        dbg("place: giving the focus back to '" + PREVWIN->m_title + "'");
        if (const auto RES = Config::Actions::focus(PREVWIN); !RES)
            dbg("place: giving the focus back failed: " + RES.error().message);
    }

    const CBox AFTER = WIN->getWindowMainSurfaceBox();
    dbg(std::format("place: now ({:.0f},{:.0f} {:.0f}x{:.0f}), on workspace {}, floating = {}", AFTER.x, AFTER.y, AFTER.w, AFTER.h,
                    WIN->m_workspace ? WIN->m_workspace->m_id : WORKSPACE_INVALID, WIN->m_isFloating));

    g_pHyprRenderer->damageMonitor(mon);
}

// Places WIN where the top view point P is, on D (moved there first if D changed during
// the drag). Nothing is done if it would land on its own current spot.
static void hyprdropPlaceInTop(PHLMONITOR mon, PHLWINDOW WIN, const Vector2D& P) {
    const auto     L      = hyprdropLayout(mon->m_size);
    const Vector2D ORIGIN = mon->m_position + g_hyprdropCaptureUsable.pos();
    const Vector2D DROP   = ORIGIN + hyprdropViewToUsable(g_hyprdropTopID, L.top, P);                         // cursor point on the real layout
    const Vector2D CORNER = ORIGIN + hyprdropViewToUsable(g_hyprdropTopID, L.top, P - g_hyprdropDragOffset); // window corner on the real layout

    const WORKSPACEID FROM = WIN->m_workspace ? WIN->m_workspace->m_id : WORKSPACE_INVALID;
    if (FROM != g_hyprdropTopID) {
        dbg(std::format("place: window is on workspace {}, top view shows {}: moving it there first", FROM, g_hyprdropTopID));
        if (!hyprdropMoveWindowSilently(mon, WIN, g_hyprdropTopID))
            return;
    } else {
        const CBox BOX = WIN->getWindowMainSurfaceBox();
        if (WIN->m_isFloating ? CORNER.distance(BOX.pos()) < 1.0 : BOX.containsPoint(DROP)) {
            dbg("place: drop point is on the window's own spot, nothing to do");
            return;
        }
    }

    hyprdropPlaceAt(mon, WIN, DROP, CORNER);
}

// Moves WIN to workspace `id` silently and keeps the overview open.
static void hyprdropMoveAndStay(PHLMONITOR mon, PHLWINDOW WIN, WORKSPACEID id) {
    hyprdropMoveWindowSilently(mon, WIN, id);
    g_hyprdropSuperDrag = false; // the overview now behaves like toggle mode
    dbg("release: staying open; to follow the window, double click it or its workspace");
    g_pHyprRenderer->damageMonitor(mon);
}

// The cursor rested in the top view during a drag: place the window there for real.
void hyprdropOnPreviewTimer() {
    const auto WIN = g_hyprdropDragWin.lock();
    const auto MON = Desktop::focusState()->monitor();
    if (!g_hyprdropOpen || !WIN || !MON)
        return;
    if (!hyprdropLayout(MON->m_size).top.containsPoint(g_hyprdropPointer))
        return;

    if (!g_hyprdropPreviewDone) {
        g_hyprdropPreviewDone   = true;
        g_hyprdropPreviewFromWS = WIN->m_workspace ? WIN->m_workspace->m_id : WORKSPACE_INVALID;
        g_hyprdropPreviewFromBox = WIN->getWindowMainSurfaceBox();
        dbg(std::format("preview: first placement, window was on workspace {} at ({:.0f},{:.0f})", g_hyprdropPreviewFromWS, g_hyprdropPreviewFromBox.x,
                        g_hyprdropPreviewFromBox.y));
    }
    dbg(std::format("preview: placing at ({:.0f},{:.0f}) in the top view", g_hyprdropPointer.x, g_hyprdropPointer.y));
    hyprdropPlaceInTop(MON, WIN, g_hyprdropPointer);
}

// Drag cancelled after preview placements: put the window back where it was (workspace,
// then position; approximate for tiled windows).
void hyprdropRestorePreview(PHLMONITOR mon, PHLWINDOW WIN) {
    if (!g_hyprdropPreviewDone || !WIN || !mon)
        return;
    g_hyprdropPreviewDone = false;

    dbg(std::format("preview: cancelled, putting '{}' back on workspace {}", WIN->m_title, g_hyprdropPreviewFromWS));
    const WORKSPACEID NOW = WIN->m_workspace ? WIN->m_workspace->m_id : WORKSPACE_INVALID;
    if (NOW != g_hyprdropPreviewFromWS) {
        if (!hyprdropMoveWindowSilently(mon, WIN, g_hyprdropPreviewFromWS))
            return;
    }
    hyprdropPlaceAt(mon, WIN, g_hyprdropPreviewFromBox.pos() + g_hyprdropPreviewFromBox.size() / 2.0, g_hyprdropPreviewFromBox.pos());
}

// A drag starts: no preview placement yet.
void hyprdropResetPreview() {
    g_hyprdropPreviewDone = false;
    if (g_hyprdropPreviewTimer && g_hyprdropPreviewTimer->armed()) {
        g_hyprdropPreviewTimer->updateTimeout(std::nullopt);
        g_pEventLoopManager->scheduleRecalc();
    }
}

void hyprdropOnRelease(PHLMONITOR mon) {
    const int TILEPRESS  = g_hyprdropTilePress;
    g_hyprdropTilePress  = -1;
    const bool TRASHPRESS = g_hyprdropTrashPress;
    g_hyprdropTrashPress  = false;

    const auto WIN = g_hyprdropDragWin.lock();
    if (!WIN && TRASHPRESS) {
        if (g_hyprdropTrashHover) {
            g_hyprdropDeleteMode = !g_hyprdropDeleteMode;
            dbg(std::string("release: delete mode ") + (g_hyprdropDeleteMode ? "on" : "off"));
            g_pHyprRenderer->damageMonitor(mon);
        } else
            dbg("release: pressed on the trash but released elsewhere, ignored");
        return;
    }
    if (!WIN) {
        // Pressed and released on the same tile. Mouse: go to that workspace (hovering
        // shows it). Finger / stylus: show it; a second tap on it goes there.
        if (TILEPRESS >= 0 && TILEPRESS == g_hyprdropHover && TILEPRESS < (int)g_hyprdropTiles.size()) {
            const WORKSPACEID ID  = g_hyprdropTiles[TILEPRESS];
            const auto        NOW = std::chrono::steady_clock::now();
            if (g_hyprdropPressIsMouse || (ID == g_hyprdropTileTapID && NOW - g_hyprdropTileTapTime < HYPRDROP_DOUBLE_TAP_DELAY)) {
                g_hyprdropTileTapID = WORKSPACE_INVALID;
                hyprdropGoToWorkspace(mon, ID);
                return;
            }
            g_hyprdropTileTapID   = ID;
            g_hyprdropTileTapTime = NOW;
            hyprdropShowOnTop(TILEPRESS, "tap");
        }
        else if (TILEPRESS >= 0)
            dbg("release: pressed on tile " + std::to_string(TILEPRESS) + " but released on tile " + std::to_string(g_hyprdropHover) + ", ignored");
        else if (g_hyprdropBackgroundPress) {
            g_hyprdropBackgroundPress = false;
            const auto L              = hyprdropLayout(mon->m_size);
            if (!L.top.containsPoint(g_hyprdropPointer) && hyprdropTileAt(L, g_hyprdropPointer) < 0) {
                dbg("release: click on the background, closing");
                hyprdropClose(mon);
            } else
                dbg("release: pressed on the background but released elsewhere, ignored");
        } else
            dbg("release: no drag in progress");
        return;
    }

    hyprdropCancelDrag();

    const Vector2D P    = g_hyprdropPointer;
    const auto     L    = hyprdropLayout(mon->m_size);
    const int      TILE = g_hyprdropHover;

    const bool ONTILE = TILE >= 0 && TILE < (int)g_hyprdropTiles.size();

    // Dropped on the trash: the window is asked to close (it may still ask to save).
    // Drag mode closes the overview, as a quick drop does; otherwise it stays open.
    if (!L.trash.empty() && L.trash.containsPoint(P)) {
        dbg("release: on the trash, closing '" + WIN->m_title + "'");
        if (const auto RES = Config::Actions::closeWindow(WIN); !RES)
            dbg("release: closing the window failed: " + RES.error().message);
        if (g_hyprdropSuperDrag)
            hyprdropClose(mon);
        else
            g_pHyprRenderer->damageMonitor(mon);
        return;
    }

    if (g_hyprdropSuperDrag) {
        if (ONTILE && g_hyprdropTiles[TILE] == g_hyprdropDwellID) {
            dbg("super drag: released on tile " + std::to_string(TILE) + " after dwelling on it");
            hyprdropMoveAndStay(mon, WIN, g_hyprdropTiles[TILE]);
        } else if (ONTILE) {
            dbg("super drag: released on tile " + std::to_string(TILE) + " (workspace " + std::to_string(g_hyprdropTiles[TILE]) + ")");
            hyprdropMoveWindowTo(mon, WIN, g_hyprdropTiles[TILE]);
        } else if (L.top.containsPoint(P)) {
            dbg("super drag: released on the top view (workspace " + std::to_string(g_hyprdropTopID) + ")");
            hyprdropPlaceInTop(mon, WIN, P);
            // On A it is just a move like Hyprland's: close. Elsewhere, stay to follow or not.
            if (g_hyprdropTopID == g_hyprdropActiveID)
                hyprdropClose(mon);
            else
                hyprdropMoveAndStay(mon, WIN, g_hyprdropTopID);
        } else {
            dbg("super drag: released outside the tiles, cancelled");
            hyprdropRestorePreview(mon, WIN);
            hyprdropClose(mon);
        }
        return;
    }

    if (P.distance(g_hyprdropPressPos) < HYPRDROP_CLICK_SLOP) {
        dbg(std::format("release: click (moved {:.1f} px)", P.distance(g_hyprdropPressPos)));
        g_pHyprRenderer->damageMonitor(mon);
        hyprdropOnWindowClick(mon, WIN);
        return;
    }

    if (ONTILE && g_hyprdropTiles[TILE] == g_hyprdropDwellID) {
        dbg("release: on tile " + std::to_string(TILE) + " after dwelling on it");
        hyprdropMoveAndStay(mon, WIN, g_hyprdropTiles[TILE]);
        return;
    }

    WORKSPACEID targetID = WORKSPACE_INVALID;
    if (TILE >= 0 && TILE < (int)g_hyprdropTiles.size()) {
        targetID = g_hyprdropTiles[TILE];
        dbg("release: on tile " + std::to_string(TILE) + " (workspace " + std::to_string(targetID) + ")");
    } else if (L.top.containsPoint(P)) {
        // The window comes from the top view, so it is already on D: place it where it was
        // released, and stay open to rearrange other windows.
        dbg("release: on the top view (workspace " + std::to_string(g_hyprdropTopID) + "), placing, staying open");
        hyprdropPlaceInTop(mon, WIN, P);
        g_pHyprRenderer->damageMonitor(mon);
        return;
    } else {
        dbg("release: neither a tile nor the top view, drag cancelled");
        hyprdropRestorePreview(mon, WIN);
        g_pHyprRenderer->damageMonitor(mon);
        return;
    }

    hyprdropMoveWindowTo(mon, WIN, targetID);
}

// Moves WIN to workspace targetID silently, creating the workspace if needed. Doesn't
// close the overview. Returns false if nothing could be done.
static bool hyprdropMoveWindowSilently(PHLMONITOR mon, PHLWINDOW WIN, WORKSPACEID targetID) {
    if (targetID == WORKSPACE_INVALID) {
        dbg("release: no valid target workspace");
        return false;
    }

    auto WS = hyprdropFindWorkspace(targetID);
    if (WS && WIN->m_workspace == WS) {
        dbg("release: window is already on workspace " + std::to_string(WS->m_id) + ", nothing to move");
        return true;
    }

    if (!WS) {
        // Only created here, when a window is actually dropped on it. Same as
        // movetoworkspacesilent to a new id: named after its id (or special:magic), not empty.
        const auto NAME = hyprdropWorkspaceName(targetID);
        dbg("release: workspace " + std::to_string(targetID) + " (" + NAME + ") doesn't exist, creating it");
        WS = State::workspaceState()->create(targetID, mon->m_id, NAME, false);
        if (!WS) {
            dbg("release: creating workspace " + std::to_string(targetID) + " failed");
            return false;
        }
        dbg(std::format("release: workspace {} ({}) created, special = {}", WS->m_id, WS->m_name, WS->m_isSpecialWorkspace));
        if (targetID == g_hyprdropMagicID && !WS->m_isSpecialWorkspace)
            dbg("release: WARNING " + HYPRDROP_MAGIC_NAME + " was created as a normal workspace");
    }

    dbg(std::format("release: moving '{}' to workspace {}", WIN->m_title, WS->m_id));
    const auto RES = Config::Actions::moveToWorkspace(WS, true, WIN);
    if (!RES) {
        dbg("release: moveToWorkspace failed: " + RES.error().message);
        return false;
    }
    dbg("release: move ok");
    return true;
}

// Same, then closes the overview.
static void hyprdropMoveWindowTo(PHLMONITOR mon, PHLWINDOW WIN, WORKSPACEID targetID) {
    hyprdropMoveWindowSilently(mon, WIN, targetID);
    hyprdropClose(mon);
}

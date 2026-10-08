#pragma once

// All standard headers BEFORE the macro, otherwise they get parsed with "protected" redefined.
#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// Like hyprexpo: beginRender/renderWorkspace are protected in 0.56 (and CWindow::m_suspended private).
#define protected public
#define private public
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#undef protected
#undef private

#include <drm_fourcc.h>
#include <hyprgraphics/image/Image.hpp>
#include <linux/input-event-codes.h>

// Shared state and functions of hyprdrop. Each .cpp keeps its own state static; what is
// here is used by several of them.
//
// Vocabulary used everywhere:
//   top view  the large view of one workspace, at the top
//   strip     the bottom band with one tile per workspace
//   D         the workspace shown in the top view (g_hyprdropTopID)
//   A         the workspace that was active when the overview opened (g_hyprdropActiveID)

inline HANDLE PHANDLE        = nullptr;
inline bool   g_hyprdropOpen = false; // whether the overview is open (it may still be closing: hyprdropVisible())

// The monitor the overview opened on. It stays there (drawing, input, captures) even if
// the focus moves to another monitor: see hyprdropMonitor().
inline PHLMONITORREF g_hyprdropMon;

// ---------------------------------------------------------------- log

// Debug log, off by default (plugin:hyprdrop:debug = true turns it on). It holds window
// titles, so it goes to $XDG_RUNTIME_DIR (only readable by the user), cleared at each load.
inline SP<Config::Values::CBoolValue> g_hyprdropDebug;

inline std::string hyprdropLogPath() {
    const char* DIR = getenv("XDG_RUNTIME_DIR");
    return std::string(DIR && *DIR ? DIR : "/tmp") + "/hyprdrop.log";
}

inline void dbg(const std::string& s) {
    if (g_hyprdropDebug && g_hyprdropDebug->value())
        std::ofstream(hyprdropLogPath(), std::ios::app) << s << "\n";
}

// ---------------------------------------------------------------- captures

// Framebuffer type, deduced from createFB so we don't have to guess its namespace.
using HyprdropFB = decltype(g_pHyprRenderer->createFB());

// One window rendered alone (renderWindow) into a monitor-sized framebuffer, at its
// real position, on a transparent background. No bar, wallpaper or other workspace.
struct SHyprdropWinCapture {
    PHLWINDOWREF win;
    HyprdropFB   fb;
    CBox         box; // window box at capture time, logical, relative to the usable area's corner
    WORKSPACEID  ws = WORKSPACE_INVALID;
    // The monitor it was rendered on (its workspace's, maybe not the overview's): logical
    // size, and its usable area's corner (monitor-local). The texture covers that monitor.
    Vector2D monSize;
    Vector2D usablePos;
};

// Captures by workspace id, in drawing order (bottom to top), redone on every frame while
// the overview is visible. A workspace that doesn't exist or has no windows has no entry.
inline std::map<WORKSPACEID, std::vector<SHyprdropWinCapture>> g_hyprdropCaptures;
// Usable area of the captured monitor (monitor minus the space reserved by the bar),
// logical, relative to the monitor. Views show this area, not the whole monitor.
inline CBox   g_hyprdropCaptureUsable;
inline double g_hyprdropCaptureScale = 1.0;   // monitor scale at capture (physical px per logical px)
inline bool   g_hyprdropCapturing    = false; // a capture is rendering: the overview must not draw
inline bool   g_hyprdropLive         = false; // per-frame recapture: capture logs are muted

// The wallpaper alone (background layer surfaces), drawn as the overview's background.
inline HyprdropFB g_hyprdropWallpaperFB;
inline bool       g_hyprdropHasWallpaper = false;

// Per-workspace fit: usable point p -> p * scale + offset. Identity, except for special
// workspaces, whose windows are scaled up to fill the view: their gaps_out (and any
// special scale factor) would otherwise leave a wide empty margin. Computed at capture.
struct SHyprdropFit {
    double   scale = 1.0;
    Vector2D offset;
    Vector2D gap = {-1, -1}; // extra space per axis (logical px, after the fit); -1 = not computed
    // Global position of the usable area's corner on the workspace's monitor: where a point
    // of the view lands on the real layout. Unset = the overview's monitor.
    std::optional<Vector2D> origin;
};
inline std::map<WORKSPACEID, SHyprdropFit> g_hyprdropFits;

// Space wanted between two windows in every view, in physical px: what two windows would
// have with a gaps_in of HYPRDROP_GAPS_IN_REF, plus HYPRDROP_WINDOW_GAP. Each workspace's
// real gaps_in (workspace rule, else general:gaps_in) is read at capture and only the
// missing part is added, so changing gaps in hyprland.lua keeps the same look.
inline constexpr double HYPRDROP_GAPS_IN_REF = 10.0;
inline constexpr double HYPRDROP_WINDOW_GAP  = 50.0;

// Where a captured window lands when its workspace is drawn in a view: `tex` is the box
// of its whole (monitor-sized) texture, `win` the box of the window itself. Drawing and
// hit-testing both use it.
struct SHyprdropPlaced {
    CBox tex;
    CBox win;
};

// ---------------------------------------------------------------- workspaces

// Workspace ids of the strip's tiles: 1..10, A if outside that range, special:magic last.
inline std::vector<WORKSPACEID> g_hyprdropTiles;
inline WORKSPACEID              g_hyprdropTopID    = WORKSPACE_INVALID; // D (it may not exist yet)
inline WORKSPACEID              g_hyprdropActiveID = WORKSPACE_INVALID; // A, its tile is marked

// The special workspace with its own tile, on a second row under the numbered ones. Its id
// is resolved when the overview opens: the existing one, or the id Hyprland would give it
// (it is only created when a window is dropped on it).
inline const std::string HYPRDROP_MAGIC_NAME = "special:magic";
inline WORKSPACEID       g_hyprdropMagicID   = WORKSPACE_INVALID;

// What D is when the overview opens.
enum eHyprdropOpenMode : uint8_t {
    HYPRDROP_OPEN_MAGIC,   // toggle_hidden(): D = special:magic (or A if it is empty), to fetch a window back
    HYPRDROP_OPEN_CURRENT, // toggle_current(): D = A
};
inline eHyprdropOpenMode g_hyprdropOpenMode = HYPRDROP_OPEN_MAGIC;

// Numbered workspace shown before going down to special:magic with the keyboard.
inline WORKSPACEID g_hyprdropBeforeMagicID = WORKSPACE_INVALID;

// ---------------------------------------------------------------- layout

// A single layout function: both drawing AND hit-testing use it, so what you see is
// exactly what you can click.
struct SHyprdropLayout {
    CBox              top;
    CBox              strip;
    std::vector<CBox> tiles;
    CBox              activeBar; // bar under A's tile, empty if A has no tile
    CBox              trash;     // next to special:magic's tile: dropping a window there closes it
    std::vector<CBox> labels;    // under each tile (same order): where its name is written
};

// ---------------------------------------------------------------- pointer and drag

// Position of whatever drives the overview, relative to the monitor: the mouse cursor, or
// the finger on the touchscreen while one is down. A touch doesn't move the cursor.
inline Vector2D g_hyprdropPointer;
// Touchscreen: id of the finger driving the overview, -1 = none. Only the first finger
// counts; while it is down, mouse moves are ignored (Hyprland also emits one on touch).
inline int32_t  g_hyprdropTouchID = -1;
inline int      g_hyprdropHover   = -1;    // tile under the pointer, -1 = none
inline bool     g_hyprdropTrashHover = false; // pointer on the trash
// Delete mode, toggled by a click on the trash: a single click on a window of the top view
// closes it, and the overview stays open. Off whenever the overview opens or closes.
inline bool     g_hyprdropDeleteMode = false;

// Press position, to tell a click (moved less than HYPRDROP_CLICK_SLOP) from a drag.
inline constexpr double HYPRDROP_CLICK_SLOP = 4.0;
inline Vector2D         g_hyprdropPressPos;
// The current press comes from the mouse (false: finger or stylus).
inline bool             g_hyprdropPressIsMouse = true;
// Last tap on a tile (it already showed it): a second tap on it goes to that workspace.
inline WORKSPACEID      g_hyprdropTileTapID = WORKSPACE_INVALID;

// Pending single click on a window, waiting for a possible second one.
inline SP<CEventLoopTimer> g_hyprdropClickTimer;
inline PHLWINDOWREF        g_hyprdropClickWin;

// Window held by the left button. It is a drag (ghost, tile hover, live preview) only once
// the pointer moved more than HYPRDROP_CLICK_SLOP: hyprdropDragging().
inline PHLWINDOWREF g_hyprdropDragWin;
inline bool         g_hyprdropDragMoved = false;
inline Vector2D     g_hyprdropDragOffset; // pointer - ghost corner, in overview coordinates
inline Vector2D     g_hyprdropDragSize;   // ghost size, in overview coordinates
// Drag mode: hl.plugin.hyprdrop.drag() (Super + left press on a desktop window) opens the
// overview with that window already dragged.
inline bool         g_hyprdropSuperDrag = false;
// Workspace of the last tile the drag dwelled on: dropping there keeps the overview open.
inline WORKSPACEID  g_hyprdropDwellID = WORKSPACE_INVALID;

// Dwell hover during a drag: resting on a tile shows it in the top view. A still cursor
// generates no events, so an event loop timer is used.
inline constexpr double    HYPRDROP_HOVER_SLOP = 4.0; // moving more than this restarts the delay
inline SP<CEventLoopTimer> g_hyprdropHoverTimer;
inline Vector2D            g_hyprdropHoverAnchor;         // pointer position when the delay started
inline bool                g_hyprdropHoverLocked = false; // tile already shown: wait until the pointer leaves it

// Live preview: resting in the top view during a drag really places the window there.
inline SP<CEventLoopTimer> g_hyprdropPreviewTimer;
inline CBox                g_hyprdropPreviewFromBox; // where it was before the first placement, global logical
// The cursor is warped during a native placement: the mouse moves it causes are ignored.
inline bool                g_hyprdropWarping = false;

// plugin:hyprdrop:zqsd: the keys at the W A S D positions (Z Q S D in AZERTY) also navigate.
inline SP<Config::Values::CBoolValue> g_hyprdropZqsd;
// plugin:hyprdrop:icon_theme: icon theme for app icons; empty = the desktop's (GTK, KDE, gsettings).
inline SP<Config::Values::CStringValue> g_hyprdropIconTheme;

// This horizontal swipe already changed the workspace shown (one step per swipe).
inline bool g_hyprdropSwipeStepped = false;

// ---------------------------------------------------------------- animations

inline double g_hyprdropAnimFrom = 0.0;   // open progress when the open / close animation started
inline bool   g_hyprdropClosing  = false; // closed but still animating: drawn, input goes to the desktop

// Workspace change in the top view: the old one slides out, the new one in.
inline WORKSPACEID                           g_hyprdropSlideFromID = WORKSPACE_INVALID;
inline int                                   g_hyprdropSlideDir    = 1; // +1: the new one comes from the right
inline std::chrono::steady_clock::time_point g_hyprdropSlideStart;

// ---------------------------------------------------------------- look

// Close to Hyprspace: the wallpaper as background, transparent views, tiles on a blurred
// strip, a border in the active border color on the tile shown at the top.
inline const CHyprColor HYPRDROP_TILE_BG     = CHyprColor{1.0, 1.0, 1.0, 0.08}; // just enough to see empty tiles
inline const CHyprColor HYPRDROP_STRIP_COLOR = CHyprColor{0.0, 0.0, 0.0, 0.25}; // over the blurred wallpaper
inline const CHyprColor HYPRDROP_DROP_BORDER = CHyprColor{1.0, 1.0, 1.0, 0.9};  // tile under a drag
inline constexpr double HYPRDROP_TILE_BORDER = 2.0;                             // logical px

// Ghost of the dragged window.
inline constexpr double HYPRDROP_GHOST_FRAME = 2.0; // frame width, logical px
inline constexpr double HYPRDROP_GHOST_SCALE = 0.5; // of the window's size in the view

// Textures made once and kept until unload: app icons by class, tile labels by text.
inline std::map<std::string, SP<Render::ITexture>> g_hyprdropIcons;
inline std::map<std::string, SP<Render::ITexture>> g_hyprdropLabels;

// ---------------------------------------------------------------- shared functions

PHLWORKSPACE hyprdropFindWorkspace(WORKSPACEID id);
PHLWORKSPACE hyprdropFindWorkspaceByName(const std::string& name);
std::string hyprdropWorkspaceName(WORKSPACEID id);
void hyprdropCheckPositions(PHLMONITOR mon, PHLWORKSPACE ws);
double hyprdropEase(double t);
double hyprdropElapsedMs(std::chrono::steady_clock::time_point start);
double hyprdropOpenProgress();
void hyprdropStartOpenAnim();
bool hyprdropVisible();
const std::vector<SHyprdropWinCapture>* hyprdropCaptureOf(WORKSPACEID id);
SHyprdropFit hyprdropFitOf(WORKSPACEID id);
Vector2D hyprdropViewToUsable(WORKSPACEID id, const CBox& view, const Vector2D& P);
SHyprdropPlaced hyprdropPlace(const SHyprdropWinCapture& c, const CBox& view);
std::optional<std::pair<PHLWINDOW, SHyprdropPlaced>> hyprdropCapturedWindowAt(WORKSPACEID id, const CBox& view, const Vector2D& P);
bool hyprdropHasMagicTile();
int hyprdropActiveTile();
void hyprdropRecaptureWorkspace(PHLMONITOR mon, WORKSPACEID id);
void hyprdropCapture(PHLMONITOR mon);
SHyprdropLayout hyprdropLayout(const Vector2D& S);
int hyprdropTileAt(const SHyprdropLayout& L, const Vector2D& P);
void hyprdropDisarmHover();
void hyprdropArmHover(const Vector2D& P);
bool hyprdropDragging();
void hyprdropCancelDrag();
void hyprdropShowOnTop(int TILE, const char* why);
void hyprdropOnHoverTimer();
void hyprdropSetCursorOverride(bool on);
void hyprdropClose(PHLMONITOR mon);
void hyprdropOnMove(Event::SCallbackInfo& info);
void hyprdropSizeSuperDragGhost(PHLMONITOR mon);
void hyprdropCancelPendingClick(const char* why);
void hyprdropGoToWorkspace(PHLMONITOR mon, WORKSPACEID id);
void hyprdropOnClickTimer();
void hyprdropOnPress(PHLMONITOR mon);
void hyprdropOnPreviewTimer();
void hyprdropRestorePreview(PHLMONITOR mon, PHLWINDOW WIN);
void hyprdropResetPreview();
void hyprdropOnRelease(PHLMONITOR mon);
void hyprdropStartDrag();
void hyprdropOnButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
void hyprdropOnAxis(Event::SCallbackInfo& info);
void hyprdropOnPinch(bool begin, bool end, Event::SCallbackInfo& info);
void hyprdropOnTouchDown(const ITouch::SDownEvent& e, Event::SCallbackInfo& info);
void hyprdropOnTouchMotion(const ITouch::SMotionEvent& e, Event::SCallbackInfo& info);
void hyprdropOnTouchUp(const ITouch::SUpEvent& e, Event::SCallbackInfo& info);
void hyprdropOnTouchCancel(const ITouch::SCancelEvent& e, Event::SCallbackInfo& info);
void hyprdropOnKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info);
void hyprdropOnTabletTip(const CTablet::STipEvent& e, Event::SCallbackInfo& info);
void hyprdropToggle(eHyprdropOpenMode mode);
PHLMONITOR hyprdropMonitor();
void hyprdropRememberOpenKey();
void hyprdropOnSwipeBegin(const IPointer::SSwipeBeginEvent& e, Event::SCallbackInfo&);
void hyprdropOnSwipeUpdate(const IPointer::SSwipeUpdateEvent& e, Event::SCallbackInfo& info);
void hyprdropOnSwipeEnd(const IPointer::SSwipeEndEvent& e, Event::SCallbackInfo&);
const SHyprdropWinCapture* hyprdropFindWindowCapture(PHLWINDOW WIN);
WORKSPACEID hyprdropShownID();
void hyprdropDraw(PHLMONITOR mon);

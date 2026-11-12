// hyprflick: a two-finger sideways flick on the touchpad moves Hyprland's
// scrolling layout by one column.
//
// libinput reports two-finger swipes as scrolling, never as gestures, and
// Hyprland only matches scroll binds against physical wheels. So this plugin
// listens to raw axis events from the touchpad instead.

#define WLR_USE_UNSTABLE

#include <hyprland/src/includes.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/supplementary/WorkspaceAlgoMatcher.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#define private public
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollTapeController.hpp>
#undef private
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>

#include <cmath>
#include <algorithm>
#include <optional>
#include <chrono>
#include <linux/input-event-codes.h>

static HANDLE PHANDLE = nullptr;

static struct {
    SP<Config::Values::CBoolValue>  enabled;
    SP<Config::Values::CFloatValue> threshold;
    SP<Config::Values::CBoolValue>  invert;
    SP<Config::Values::CIntValue>   gapMs;
    SP<Config::Values::CIntValue>   bounce;
    SP<Config::Values::CBoolValue>  returnOnType;
} cfg;

// The "boing" at either end: nudge the tape past the end, then put it back.
static struct {
    SP<CEventLoopTimer> timer;
    PHLWORKSPACEREF     ws;
    double              restoreOffset = 0;
    bool                active        = false;
    std::optional<long> restoreFitMethod;
} boing;

// Hyprland snaps a negative tape offset back to 0 whenever
// scrolling:focus_fit_method is "fit" (1), which would cancel a boing past the
// left end. Switching it to "center" (0) for the length of the boing lets the
// tape go negative. Function hooks would be cleaner but are x86_64 only.
static Config::INTEGER* fitMethodPtr() {
    static const auto PFITMETHOD = CConfigValue<Config::INTEGER>("scrolling:focus_fit_method");
    return PFITMETHOD.ptr();
}

static constexpr auto BOING_HOLD = std::chrono::milliseconds(120);


// One flick is a run of finger scroll events without a long gap or a stop.
static struct {
    double   h = 0, v = 0;
    bool     fired     = false;
    uint32_t lastTimeMs = 0;
} flick;

using Layout::Tiled::CScrollingAlgorithm;

static PHLWORKSPACE scrollingWorkspace() {
    const auto MON = Desktop::focusState()->monitor();
    if (!MON)
        return nullptr;

    const auto WS = MON->m_activeWorkspace;
    if (!WS || !WS->m_space)
        return nullptr;

    const auto ALGO = WS->m_space->algorithm();
    if (!ALGO || !ALGO->tiledAlgo())
        return nullptr;

    if (Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(ALGO->tiledAlgo().get()) != "scrolling")
        return nullptr;

    return WS;
}

static CScrollingAlgorithm* scrollingAlgoFor(PHLWORKSPACE ws) {
    if (!ws || !ws->m_space || !ws->m_space->algorithm() || !ws->m_space->algorithm()->tiledAlgo())
        return nullptr;

    const auto ALGO = ws->m_space->algorithm()->tiledAlgo().get();
    if (Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(ALGO) != "scrolling")
        return nullptr;

    auto* scrolling = static_cast<CScrollingAlgorithm*>(ALGO);
    if (!scrolling->m_scrollingData || !scrolling->m_scrollingData->controller)
        return nullptr;

    return scrolling;
}

static void endBoing() {
    if (!boing.active)
        return;
    boing.active = false;
    boing.timer->updateTimeout(std::nullopt);

    auto* scrolling = scrollingAlgoFor(boing.ws.lock());
    if (scrolling)
        scrolling->m_scrollingData->controller->setOffset(boing.restoreOffset);

    if (boing.restoreFitMethod) {
        *fitMethodPtr() = *boing.restoreFitMethod;
        boing.restoreFitMethod.reset();
    }

    if (scrolling)
        scrolling->m_scrollingData->recalculate();
}

static void startBoing(PHLWORKSPACE ws, CScrollingAlgorithm* scrolling, bool forward) {
    const int DIST = cfg.bounce->value();
    if (DIST <= 0)
        return;

    auto& tape          = *scrolling->m_scrollingData->controller;
    boing.ws            = ws;
    boing.restoreOffset = tape.getOffset();
    boing.active        = true;

    const double OUT = boing.restoreOffset + (forward ? DIST : -DIST);
    if (OUT < 0) {
        boing.restoreFitMethod = *fitMethodPtr();
        *fitMethodPtr()        = 0;
    }

    tape.setOffset(boing.restoreOffset + (forward ? DIST : -DIST));
    scrolling->m_scrollingData->recalculate();
    boing.timer->updateTimeout(BOING_HOLD);
}

// Move the view, never the focus. Forward lines up the first column that
// sticks out past the right edge with that edge; backward lines up the first
// column hidden past the left edge with the left edge. At either end of the
// strip there is nothing hidden, so it boings instead.
static void scrollOneColumn(PHLWORKSPACE ws, bool forward) {
    static const auto PFSONONE = CConfigValue<Config::INTEGER>("scrolling:fullscreen_on_one_column");

    endBoing(); // finish any bounce in progress so offsets start from rest

    auto* scrolling = scrollingAlgoFor(ws);
    if (!scrolling)
        return;

    auto&        tape   = *scrolling->m_scrollingData->controller;
    const auto   USABLE = scrolling->usableArea();
    const bool   FSONE  = *PFSONONE;
    const double VIEW   = tape.isPrimaryHorizontal() ? USABLE.w : USABLE.h;
    const size_t N      = tape.stripCount();

    if (N == 0 || VIEW <= 0)
        return;

    constexpr double EPS    = 2.0;
    const double     OFFSET = tape.getOffset();
    const double     MAXOFF = std::max(0.0, tape.calculateMaxExtent(USABLE, FSONE) - VIEW);

    std::optional<double> target;

    if (forward) {
        for (size_t i = 0; i < N; ++i) {
            const double END = tape.calculateStripStart(i, USABLE, FSONE) + tape.calculateStripSize(i, USABLE, FSONE);
            if (END > OFFSET + VIEW + EPS) {
                target = END - VIEW;
                break;
            }
        }
    } else {
        for (size_t i = N; i-- > 0;) {
            const double START = tape.calculateStripStart(i, USABLE, FSONE);
            if (START < OFFSET - EPS) {
                target = START;
                break;
            }
        }
    }

    if (target)
        target = std::clamp(*target, 0.0, MAXOFF);

    // Nothing hidden that way, or already lined up: boing instead.
    if (!target || std::abs(*target - OFFSET) < EPS) {
        startBoing(ws, scrolling, forward);
        return;
    }

    tape.setOffset(*target);
    scrolling->m_scrollingData->recalculate();
}

static void onAxis(IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
    if (!cfg.enabled->value() || e.source != WL_POINTER_AXIS_SOURCE_FINGER)
        return;

    const auto WS = scrollingWorkspace();
    if (!WS)
        return;

    const bool HORIZONTAL = e.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL;

    // A zero delta is libinput's "fingers lifted" stop event.
    if (e.delta == 0.0 || e.timeMs - flick.lastTimeMs > (uint32_t)cfg.gapMs->value())
        flick = {};
    flick.lastTimeMs = e.timeMs;

    if (!HORIZONTAL) {
        flick.v += e.delta; // vertical scrolling still goes to the app
        return;
    }

    // Swallow all sideways touchpad scrolling on scrolling workspaces.
    info.cancelled = true;

    if (flick.fired || e.delta == 0.0)
        return;

    flick.h += e.delta;

    // Only fire when the motion is clearly sideways, so a slightly diagonal
    // vertical scroll doesn't move the tape.
    if (std::abs(flick.h) < cfg.threshold->value() || std::abs(flick.h) < 2 * std::abs(flick.v))
        return;

    flick.fired = true;

    bool forward = flick.h > 0;
    if (cfg.invert->value())
        forward = !forward;

    g_pEventLoopManager->doLater([ws = PHLWORKSPACEREF{WS}, forward] {
        if (const auto W = ws.lock())
            scrollOneColumn(W, forward);
    });
}

// Typing goes to the focused window even after a flick has slid it out of
// view. So when a key press is headed for that window, bring its column back
// first, the same way Hyprland frames a window when focus moves to it.
// Only keys that type or move a cursor count. Codes below KEY_MUTE cover the
// main block, F1-F12, arrows, the edit keys and the keypad; above that are
// volume, brightness and media keys, which are bound to the system. Print
// (KEY_SYSRQ) is a screenshot, and modifiers alone type nothing.
static bool isTypingKey(uint32_t keycode) {
    if (keycode >= KEY_MUTE)
        return false;
    switch (keycode) {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
        case KEY_CAPSLOCK:
        case KEY_SYSRQ: return false;
        default: return true;
    }
}

// True when the keyboard is on the focused window itself, not on a launcher,
// lock screen or other layer surface sitting above it.
static bool keyboardOnFocusedWindow() {
    const auto WINDOW = Desktop::focusState()->window();
    if (!WINDOW || !WINDOW->wlSurface())
        return false;
    const auto KBFOCUS = g_pSeatManager->m_state.keyboardFocus.lock();
    return KBFOCUS && KBFOCUS == WINDOW->wlSurface()->resource();
}

static void returnToFocused() {
    const auto WS = scrollingWorkspace();
    if (!WS)
        return;

    const auto WINDOW = Desktop::focusState()->window();
    if (!WINDOW || WINDOW->m_workspace != WS)
        return;

    auto* scrolling = scrollingAlgoFor(WS);
    if (!scrolling)
        return;

    const auto TDATA = scrolling->dataFor(WINDOW->layoutTarget(), true);
    if (!TDATA)
        return; // floating, or otherwise not on the tape

    const auto COL = TDATA->column.lock();
    if (!COL)
        return;

    endBoing();

    auto& data = *scrolling->m_scrollingData;
    if (data.visible(COL, true))
        return;

    data.centerOrFitCol(COL);
    data.recalculate();
}

static void onKey(IKeyboard::SKeyEvent e) {
    if (!cfg.enabled->value() || !cfg.returnOnType->value() || e.state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;

    // Super combos are compositor shortcuts, not typing, so leave the view
    // alone for those.
    if (!isTypingKey(e.keycode) || (g_pInputManager->getModsFromAllKBs() & HL_MODIFIER_META))
        return;

    if (!keyboardOnFocusedWindow())
        return;

    g_pEventLoopManager->doLater([] { returnToFocused(); });
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprflick] Built against different Hyprland headers than the running Hyprland. Rebuild the plugin.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[hyprflick] version mismatch");
    }

    cfg.enabled   = makeShared<Config::Values::CBoolValue>("plugin:hyprflick:enabled", "Turn two-finger flicks on or off", true);
    cfg.threshold = makeShared<Config::Values::CFloatValue>("plugin:hyprflick:threshold", "Sideways scroll distance before a flick moves the tape", 15.F,
                                                            Config::Values::SFloatValueOptions{.min = 1.F, .max = 500.F});
    cfg.invert    = makeShared<Config::Values::CBoolValue>("plugin:hyprflick:invert", "Reverse the flick direction", false);
    cfg.gapMs     = makeShared<Config::Values::CIntValue>("plugin:hyprflick:gap_ms", "Pause in milliseconds that ends one flick and starts the next", 150,
                                                          Config::Values::SIntValueOptions{.min = 10, .max = 2000});

    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.enabled);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.threshold);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.invert);
    cfg.bounce    = makeShared<Config::Values::CIntValue>("plugin:hyprflick:bounce", "Pixels to bounce past the end when there is nothing more to scroll to (0 = off)", 40,
                                                          Config::Values::SIntValueOptions{.min = 0, .max = 400});

    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.gapMs);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.bounce);

    cfg.returnOnType = makeShared<Config::Values::CBoolValue>("plugin:hyprflick:return_on_type",
                                                              "Scroll back to the focused window when you type into it while it's out of view", true);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.returnOnType);

    boing.timer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { endBoing(); }, nullptr);
    g_pEventLoopManager->addTimer(boing.timer);

    static auto P = Event::bus()->m_events.input.mouse.axis.listen([](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onAxis(e, info); });
    static auto K = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo&) { onKey(e); });

    return {"hyprflick", "Two-finger touchpad flicks move the scrolling layout one column", "Eli Finer", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    endBoing();
    if (boing.timer)
        g_pEventLoopManager->removeTimer(boing.timer);
    boing.timer.reset();
}

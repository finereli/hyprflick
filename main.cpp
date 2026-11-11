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
#define private public
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollTapeController.hpp>
#undef private
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>

#include <cmath>
#include <algorithm>
#include <optional>

static HANDLE PHANDLE = nullptr;

static struct {
    SP<Config::Values::CBoolValue>  enabled;
    SP<Config::Values::CFloatValue> threshold;
    SP<Config::Values::CBoolValue>  invert;
    SP<Config::Values::CIntValue>   gapMs;
} cfg;

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

// Move the view, never the focus. Forward lines up the first column that
// sticks out past the right edge with that edge; backward lines up the first
// column hidden past the left edge with the left edge. At either end of the
// strip there is nothing hidden, so nothing moves.
static void scrollOneColumn(PHLWORKSPACE ws, bool forward) {
    static const auto PFSONONE = CConfigValue<Config::INTEGER>("scrolling:fullscreen_on_one_column");

    if (!ws->m_space || !ws->m_space->algorithm() || !ws->m_space->algorithm()->tiledAlgo())
        return;

    const auto ALGO = ws->m_space->algorithm()->tiledAlgo().get();
    if (Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(ALGO) != "scrolling")
        return;

    auto* scrolling = static_cast<CScrollingAlgorithm*>(ALGO);
    if (!scrolling->m_scrollingData || !scrolling->m_scrollingData->controller)
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

    if (!target)
        return;

    tape.setOffset(std::clamp(*target, 0.0, MAXOFF));
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
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.gapMs);

    static auto P = Event::bus()->m_events.input.mouse.axis.listen([](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onAxis(e, info); });

    return {"hyprflick", "Two-finger touchpad flicks move the scrolling layout one column", "Eli Finer", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {}

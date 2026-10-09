/**
 * @file main.cpp
 * @brief noled — Screen-off overlay for Nintendo Switch.
 *
 * Turns off the screen backlight during idle periods (e.g. downloads, file
 * transfers).  Prevents OLED burn-in and saves battery on all Switch models.
 * Two activation modes are available:
 *
 *   Quick tap  → Screen off only. Device may auto-sleep normally.
 *   Long press → Screen off + stay awake. Periodically signals user activity
 *                to the idle service so the device never auto-sleeps.
 *
 * When the device sleeps and wakes (e.g. power button), the overlay detects
 * the time gap between frames and exits cleanly without flashing the screen.
 *
 * @author  kemalsanli
 * @license GPL-2.0
 * @see     https://github.com/kemalsanli/noled
 */

#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <memory>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static constexpr const char *APP_VERSION = "1.0.0-diag";

/// How long the user must hold A to activate stay-awake mode.
static constexpr u64 HOLD_DURATION_MS = 1500;

/// Interval between idle:sys keep-alive pings (must be well below the
/// shortest system auto-sleep timeout, which is 1 minute).
static constexpr u64 KEEPALIVE_INTERVAL_MS = 30'000;

/// Frame-gap threshold for detecting that the device slept and woke up.
static constexpr u64 SLEEP_GAP_MS = 2000;

static constexpr s32 BAR_HEIGHT = 8;
static constexpr s32 BAR_MARGIN = 40;

// ---------------------------------------------------------------------------
// Application state machine
// ---------------------------------------------------------------------------

/// Overlay lifecycle phases.
///   Detecting → user is deciding (quick tap vs long press)
///   Confirmed → long press completed, waiting for A release
///   Blackout  → screen is off, overlay is idle
enum class AppState { Detecting, Confirmed, Blackout };

// ---------------------------------------------------------------------------
// Global service state
// ---------------------------------------------------------------------------

static bool    g_lblAvailable    = false;  ///< lbl backlight service ready
static bool    g_backlightOff    = false;  ///< backlight is currently off
static bool    g_sleepLockActive = false;  ///< stay-awake mode is engaged
static Service g_idleSrv;                  ///< idle:sys service handle
static bool    g_idleAvailable   = false;  ///< idle:sys session is open

// --- DIAGNOSTIC: record what the backlight service actually reports --------
static Result  g_diagInitRc   = 0;   ///< result of lblInitialize()
static Result  g_diagOffRc    = 0;   ///< result of last lblSwitchBacklightOff()
static Result  g_diagStatRc   = 0;   ///< result of lblGetBacklightSwitchStatus()
static LblBacklightSwitchStatus g_diagStatus = LblBacklightSwitchStatus_Enabled;
static bool    g_diagDimming  = false; ///< lblIsDimmingEnabled()

// ---------------------------------------------------------------------------
// Service helpers
// ---------------------------------------------------------------------------

/// Sends a "user is active" signal to the system's idle service (command 5).
/// Resets the auto-sleep countdown without changing any system settings.
static void keepAlive() {
    if (g_idleAvailable)
        serviceDispatch(&g_idleSrv, 5);
}

/// Restores backlight and resets sleep-lock flag.
/// Safe to call multiple times; skips work if nothing needs restoring.
static void restoreAll() {
    if (g_backlightOff && g_lblAvailable) {
        lblSwitchBacklightOn(0);
        g_backlightOff = false;
    }
    g_sleepLockActive = false;
}

// ===========================================================================
// NoledElement — full-screen custom element that owns the state machine
// ===========================================================================

class NoledElement : public tsl::elm::Element {
public:
    AppState state() const { return m_state; }

    /// Drives the state machine based on current held keys.
    /// Called from NoledGui::handleInput so key data is always fresh.
    void processInput(u64 keysHeld) {
        const bool aHeld = (keysHeld & HidNpadButton_A) != 0;

        if (m_state == AppState::Confirmed) {
            if (!aHeld)
                m_state = AppState::Blackout;
            return;
        }

        if (m_state != AppState::Detecting)
            return;

        // First frame after overlay launch: A already released → quick tap.
        if (m_firstInput) {
            m_firstInput = false;
            if (!aHeld) {
                m_state = AppState::Blackout;
                return;
            }
            m_holdActive    = true;
            m_holdStartTick = armGetSystemTick();
            return;
        }

        if (!m_holdActive)
            return;

        // A released before threshold → quick tap.
        if (!aHeld) {
            m_state = AppState::Blackout;
            return;
        }

        // Update progress toward the stay-awake threshold.
        const u64 elapsedMs = armTicksToNs(armGetSystemTick() - m_holdStartTick) / 1'000'000;
        m_percentage = std::min(100, static_cast<int>((elapsedMs * 100) / HOLD_DURATION_MS));

        if (m_percentage >= 100) {
            g_sleepLockActive = true;
            m_state = AppState::Confirmed;
        }
    }

    void draw(tsl::gfx::Renderer *renderer) override {
        renderer->fillScreen(tsl::Color{0x0, 0x0, 0x0, 0xF});

        if (m_state == AppState::Detecting && m_holdActive)
            drawHolding(renderer);
        else if (m_state == AppState::Confirmed)
            drawConfirmed(renderer);

        if (m_state == AppState::Blackout) {
            // Quick-tap: turn off once.  Stay-awake: keep forcing off every
            // frame so the backlight re-darkens after a manual power-button wake.
            if (g_lblAvailable && (!g_backlightOff || g_sleepLockActive)) {
                g_diagOffRc = lblSwitchBacklightOff(0);
                g_backlightOff = true;
            }

            // DIAGNOSTIC: ask the service what state it thinks it is in.
            if (g_lblAvailable) {
                g_diagStatRc = lblGetBacklightSwitchStatus(&g_diagStatus);
                lblIsDimmingEnabled(&g_diagDimming);
            }

            // Drawn on the overlay panel. If the backlight really is off you
            // won't see this; if only the panel goes black, read the codes.
            char line[96];
            renderer->drawString("noled diagnostics", false, 20, 140, 18,
                tsl::Color{0xF, 0xF, 0x0, 0xF});
            snprintf(line, sizeof(line), "lbl available: %s", g_lblAvailable ? "yes" : "NO");
            renderer->drawString(line, false, 20, 175, 16, tsl::Color{0xF, 0xF, 0xF, 0xF});
            snprintf(line, sizeof(line), "init rc: 0x%X", g_diagInitRc);
            renderer->drawString(line, false, 20, 200, 16, tsl::Color{0xF, 0xF, 0xF, 0xF});
            snprintf(line, sizeof(line), "off rc: 0x%X", g_diagOffRc);
            renderer->drawString(line, false, 20, 225, 16, tsl::Color{0xF, 0xF, 0xF, 0xF});
            snprintf(line, sizeof(line), "status rc: 0x%X  status: %d", g_diagStatRc, (int)g_diagStatus);
            renderer->drawString(line, false, 20, 250, 16, tsl::Color{0xF, 0xF, 0xF, 0xF});
            snprintf(line, sizeof(line), "dimming enabled: %s", g_diagDimming ? "yes" : "no");
            renderer->drawString(line, false, 20, 275, 16, tsl::Color{0xF, 0xF, 0xF, 0xF});
            renderer->drawString("(status 0=off 1=on 2=turning on 3=turning off)", false, 20, 300, 13,
                tsl::Color{0x8, 0x8, 0x8, 0xF});

            if (g_sleepLockActive) {
                const u64 now       = armGetSystemTick();
                const u64 elapsedMs = armTicksToNs(now - m_lastKeepAliveTick) / 1'000'000;
                if (m_lastKeepAliveTick == 0 || elapsedMs >= KEEPALIVE_INTERVAL_MS) {
                    keepAlive();
                    m_lastKeepAliveTick = now;
                }
            }
        }
    }

    void layout(u16, u16, u16, u16) override {
        setBoundaries(0, 0, tsl::cfg::FramebufferWidth, tsl::cfg::FramebufferHeight);
    }

private:
    AppState m_state              = AppState::Detecting;
    u64      m_holdStartTick     = 0;
    u64      m_lastKeepAliveTick = 0;
    bool     m_holdActive        = false;
    bool     m_firstInput        = true;
    int      m_percentage        = 0;

    // -- Drawing helpers ----------------------------------------------------

    /// Draws the branded header: "no" (red) + "led" (white) + version + URL.
    /// Returns the Y position below the separator line for content placement.
    s32 drawHeader(tsl::gfx::Renderer *renderer) {
        const s32 fbW = tsl::cfg::FramebufferWidth;

        // "no" in red, "led" in white
        auto [noW, noH] = renderer->drawString("no", false, 20, 50, 26,
            tsl::Color{0xF, 0x2, 0x2, 0xF});
        renderer->drawString("led", false, 20 + noW, 50, 26,
            tsl::Color{0xF, 0xF, 0xF, 0xF});

        // Version badge (smaller, gray, right of title)
        auto [ledW, ledH] = renderer->drawString("led", false, 0, 0, 26,
            tsl::Color{0x0, 0x0, 0x0, 0x0});
        char ver[16];
        snprintf(ver, sizeof(ver), "v%s", APP_VERSION);
        renderer->drawString(ver, false, 20 + noW + ledW + 10, 55, 15,
            tsl::Color{0x5, 0x5, 0x5, 0xF});

        // Author URL
        renderer->drawString("github.com/kemalsanli", false, 20, 72, 13,
            tsl::Color{0x5, 0x5, 0x5, 0xF});

        // Separator
        constexpr s32 sepY = 90;
        renderer->drawRect(20, sepY, fbW - 40, 1,
            tsl::Color{0x3, 0x3, 0x3, 0xF});

        return sepY;
    }

    /// Detection screen: shows "Screen Off" label, hold hint, and progress bar.
    void drawHolding(tsl::gfx::Renderer *renderer) {
        const s32 fbW = tsl::cfg::FramebufferWidth;
        const s32 fbH = tsl::cfg::FramebufferHeight;
        const s32 cx  = fbW / 2;

        drawHeader(renderer);

        // Title
        auto [tw, th] = renderer->drawString("Screen Off", false, 0, 0, 22,
            tsl::Color{0x0, 0x0, 0x0, 0x0});
        renderer->drawString("Screen Off", false, cx - tw / 2, fbH / 2 - 60, 22,
            tsl::Color{0xF, 0xF, 0xF, 0xF});

        // Hint
        auto [iw, ih] = renderer->drawString("Hold to keep device awake", false, 0, 0, 16,
            tsl::Color{0x0, 0x0, 0x0, 0x0});
        renderer->drawString("Hold to keep device awake", false, cx - iw / 2, fbH / 2 - 10, 16,
            tsl::Color{0x8, 0x8, 0x8, 0xF});

        // Progress bar
        const s32 barX = BAR_MARGIN;
        const s32 barY = fbH / 2 + 30;
        const s32 barW = fbW - BAR_MARGIN * 2;

        renderer->drawRect(barX, barY, barW, BAR_HEIGHT,
            tsl::Color{0x2, 0x2, 0x2, 0xF});

        const s32 fillW = (barW * m_percentage) / 100;
        if (fillW > 0)
            renderer->drawRect(barX, barY, fillW, BAR_HEIGHT,
                tsl::Color{0x0, 0xD, 0x8, 0xF});

        // Percentage text
        char pctText[8];
        snprintf(pctText, sizeof(pctText), "%d%%", m_percentage);
        auto [pw, ph] = renderer->drawString(pctText, false, 0, 0, 16,
            tsl::Color{0x0, 0x0, 0x0, 0x0});
        renderer->drawString(pctText, false, cx - pw / 2, barY + BAR_HEIGHT + 22, 16,
            tsl::Color{0x8, 0x8, 0x8, 0xF});
    }

    /// Confirmation screen: shows success message and full progress bar.
    void drawConfirmed(tsl::gfx::Renderer *renderer) {
        const s32 fbW = tsl::cfg::FramebufferWidth;
        const s32 fbH = tsl::cfg::FramebufferHeight;
        const s32 cx  = fbW / 2;

        drawHeader(renderer);

        auto [tw, th] = renderer->drawString("Screen Off + Stay Awake!", false, 0, 0, 22,
            tsl::Color{0x0, 0x0, 0x0, 0x0});
        renderer->drawString("Screen Off + Stay Awake!", false, cx - tw / 2, fbH / 2 - 40, 22,
            tsl::Color{0xA, 0xF, 0xA, 0xF});

        const s32 barX = BAR_MARGIN;
        const s32 barY = fbH / 2 + 20;
        const s32 barW = fbW - BAR_MARGIN * 2;
        renderer->drawRect(barX, barY, barW, BAR_HEIGHT,
            tsl::Color{0x0, 0xD, 0x8, 0xF});
    }
};

// ===========================================================================
// NoledGui — Tesla GUI wrapper: routes input and detects sleep/wake gaps
// ===========================================================================

class NoledGui : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        m_element = new NoledElement();
        return m_element;
    }

    void update() override {}

    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                     HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override {
        if (!m_element) return false;

        // --- Sleep/wake detection -------------------------------------------
        // If the frame gap exceeds the threshold while blacked out, the device
        // slept and woke.  Exit silently: don't touch the backlight (the system
        // handles it) and use close() so Tesla-Menu doesn't flash on screen.
        const u64 now = armGetSystemTick();
        if (m_lastFrameTick != 0 && m_element->state() == AppState::Blackout) {
            const u64 gapMs = armTicksToNs(now - m_lastFrameTick) / 1'000'000;
            if (gapMs > SLEEP_GAP_MS) {
                g_backlightOff    = false;
                g_sleepLockActive = false;
                tsl::Overlay::get()->close();
                return true;
            }
        }
        m_lastFrameTick = now;

        // --- Normal input ---------------------------------------------------
        m_element->processInput(keysHeld);

        // Any key press during blackout → restore and exit to Tesla-Menu.
        if (m_element->state() == AppState::Blackout) {
            if (keysDown != 0) {
                restoreAll();
                tsl::goBack();
                return true;
            }
        }

        return true;
    }

private:
    NoledElement *m_element       = nullptr;
    u64           m_lastFrameTick = 0;
};

// ===========================================================================
// NoledOverlay — Tesla overlay lifecycle: service init, cleanup, GUI creation
// ===========================================================================

class NoledOverlay : public tsl::Overlay {
public:
    void initServices() override {
        g_diagInitRc    = lblInitialize();
        g_lblAvailable  = R_SUCCEEDED(g_diagInitRc);
        g_idleAvailable = R_SUCCEEDED(smGetService(&g_idleSrv, "idle:sys"));
    }

    void exitServices() override {
        restoreAll();

        if (g_idleAvailable) {
            serviceClose(&g_idleSrv);
            g_idleAvailable = false;
        }
        if (g_lblAvailable)
            lblExit();
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return std::make_unique<NoledGui>();
    }
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    return tsl::loop<NoledOverlay>(argc, argv);
}

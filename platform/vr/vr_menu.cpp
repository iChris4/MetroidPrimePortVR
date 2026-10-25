// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's VR menu on the game thread (vr/vr_menu.h): reads the laser the
// XR pacing thread aimed, runs the menu, applies what it changed and hands
// Aurora the menu's image whenever what it shows changes.

#include "vr/vr_menu.h"

#include "vr/vr_log.h"
#include "vr/vr_settings.h"
#include "vr/vr_view.h"

#if defined(MP_ENABLE_OPENXR)
#include "cannon_textures.h"
#include "port_debug.h"
#include "port_log_file.h"
#include "port_paths.h"
#include "port_savestate.h"
#include "port_textures.h"
#include "vr/openxr_integration.h"
#include "vr/openxr_settings_panel.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>

#include <chrono>
#include <cstdint>
#include <string>
#endif

namespace PortVr {

#if defined(MP_ENABLE_OPENXR)
namespace {

double NowSeconds() noexcept {
    using namespace std::chrono;
    return duration< double >(steady_clock::now().time_since_epoch()).count();
}

// The menu's actions on the port: the settings file, the port's save states
// (PortSaveState, the overlay's States page) and the launcher's cannon slots.
class GameActions final : public VrMenu::Actions {
public:
    // Both wait for the settings the click changed to be stored (Flush).
    void SaveSettings() override { m_save = true; }
    void ExitGame() override {
        m_save = true;
        m_exit = true;
    }

    int StateSlot() const override {
        const int slot = PortSaveState::SelectedSlot();
        return slot >= 1 && slot <= static_cast<int>(VrMenu::kStateSlotCount) ? slot : 1;
    }
    void SelectStateSlot(int slot) override { PortSaveState::SetSelectedSlot(slot); }
    void LoadState(int slot) override { Report(PortSaveState::RequestLoad(slot)); }
    void SaveState(int slot) override { Report(PortSaveState::RequestSave(slot)); }

    // The slot saved last.
    void LoadNewestState() override {
        int newest = 0;
        int64_t newest_at = 0;
        for (int slot = 1; slot <= static_cast<int>(VrMenu::kStateSlotCount); ++slot) {
            const PortSaveState::Info info = PortSaveState::SlotInfo(slot);
            if (info.exists && (newest == 0 || info.savedAt > newest_at)) {
                newest = slot;
                newest_at = info.savedAt;
            }
        }
        if (newest == 0) {
            PORTVR_LOG() << "VR menu: LOAD NEWEST found no saved state" << std::endl;
            return;
        }
        PortSaveState::SetSelectedSlot(newest);
        Report(PortSaveState::RequestLoad(newest));
    }

    // The first empty slot, else the one saved longest ago.
    void SaveOldestState() override {
        int oldest = 0;
        int64_t oldest_at = 0;
        for (int slot = 1; slot <= static_cast<int>(VrMenu::kStateSlotCount); ++slot) {
            const PortSaveState::Info info = PortSaveState::SlotInfo(slot);
            if (!info.exists) {
                oldest = slot;
                break;
            }
            if (oldest == 0 || info.savedAt < oldest_at) {
                oldest = slot;
                oldest_at = info.savedAt;
            }
        }
        PortSaveState::SetSelectedSlot(oldest);
        Report(PortSaveState::RequestSave(oldest));
    }

    // The launcher's Cannon Textures tab, from the headset: the slot's files
    // into the managed folder of the user texture pack, which is then reloaded.
    bool ApplyCannonSlot(int slot) override {
        namespace Cannon = PrimedGunLauncher::Cannon;
        const std::string user_textures = PortTextures::UserRoot();
        if (user_textures.empty() || PortPaths::UserFolder().empty()) {
            PORTVR_LOG() << "VR menu: no user texture pack folder for the cannon textures" << std::endl;
            return false;
        }
        Cannon::Folders folders;
        folders.library = PortPaths::detail::FromUtf8(PortPaths::UserFolder()) / "primedgun" / "cannon_textures";
        if (const char* base = SDL_GetBasePath(); base != nullptr) {
            folders.shippedLibrary = PortPaths::detail::FromUtf8(base) / "primedgun" / "cannon_textures";
        }
        folders.userTextures = PortPaths::detail::FromUtf8(user_textures);
        std::string error;
        if (!Cannon::ApplySlot(folders, slot, error)) {
            PORTVR_LOG() << "VR menu: cannon texture slot " << slot << " not applied: " << error << std::endl;
            return false;
        }
        PortTextures::RequestUserPackReload();
        PORTVR_LOG() << "VR menu: cannon texture slot " << Cannon::SlotName(slot) << " applied" << std::endl;
        return true;
    }

    // DEBUG: the F1 overlay's Debug tab, which persists the audio switches the
    // same way (the settings file is marked dirty).
    void FullHealth() override { PortDebug::CheatFullHealth(); }
    void GrantEverything() override { PortDebug::CheatGrantEverything(); }
    void SetInvulnerable(bool on) override { PortDebug::SetInvulnerable(on); }
    void SetStreamedAudio(bool on) override {
        PortDebug::SetAiAudioEnabled(on);
        PortDebug::MarkVrSettingsDirty();
        m_save = true;
    }
    void SetMusyxAudio(bool on) override {
        PortDebug::SetMusyxAudioEnabled(on);
        PortDebug::MarkVrSettingsDirty();
        m_save = true;
    }
    void SetLogFile(bool on) override {
        PortDebug::SetLogFile(on);
        m_save = true;
        if (on) {
            PortLogFile::Start();
        }
    }
    void SetQuickResume(bool on) override {
        PortDebug::SetQuickResume(on);
        m_save = true;
    }
    void SetDrawTags(bool on) override { PortDebug::SetDrawTags(on); }

    // After the click's settings are stored: the requested save, then the exit
    // the way closing the window exits (the F1 overlay's Exit game).
    void Flush() {
        if (m_save) {
            m_save = false;
            PortDebug::SaveSettingsNow();
        }
        if (m_exit) {
            m_exit = false;
            OpenXRSetSettingsPanelOpen(false);
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
    }

private:
    static void Report(bool accepted) {
        if (!accepted) {
            PORTVR_LOG() << "VR menu: " << PortSaveState::LastMessage() << std::endl;
        }
    }

    bool m_save = false;
    bool m_exit = false;
};

// The game's frame rate, from its own frames: aurora_get_fps counts the
// window's presents, which a standalone headset (the only display) never makes.
struct FrameRate {
    uint64_t frames = 0;
    uint64_t counted = 0;
    double since = 0.0;
    float fps = 0.0f;

    void Tick(double now) noexcept {
        ++frames;
        if (since == 0.0) {
            since = now;
            counted = frames;
            return;
        }
        if (const double elapsed = now - since; elapsed >= 0.5) {
            fps = static_cast<float>(static_cast<double>(frames - counted) / elapsed);
            since = now;
            counted = frames;
        }
    }
};

struct Menu {
    FrameRate rate;
    VrMenu::State state;
    GameActions actions;
    bool was_open = false;
    // What the image Aurora has shows (VrMenu::ViewKey); empty when it has none.
    std::string shown;
    // DEBUG's readouts, sampled twice a second so the image is not redrawn every frame.
    VrMenu::View readouts;
    double readouts_at = 0.0;
    // The shader compilation toast has the panel's image (it shows while the menu is
    // closed), at this VrMenu::CompileToastKey.
    bool toast_shown = false;
    uint32_t toast_key = 0;
};

// The startup compile's progress (PortDebug::ShaderCompilationProgress) on the panel layer,
// which the menu has while it is open. The image is rebuilt only when the percent changes.
void UpdateCompileToast(Menu& menu, bool compiling, uint32_t done, uint32_t total) noexcept {
    if (!compiling || OpenXRSettingsPanelOpen()) {
        if (menu.toast_shown) {
            menu.toast_shown = false;
            OpenXRSetCompileToastShown(false);
            PORTVR_LOG() << "shader compilation toast hidden" << (compiling ? " (VR menu open)" : "") << std::endl;
            if (!OpenXRSettingsPanelOpen()) {
                aurora_set_stereo_panel_image(nullptr, 0, 0);
            }
        }
        return;
    }
    const uint32_t key = VrMenu::CompileToastKey(done, total);
    if (!menu.toast_shown || key != menu.toast_key) {
        const VrMenu::Pixels pixels = VrMenu::BuildCompileToastPixels(done, total);
        aurora_set_stereo_panel_image(pixels.data(), VrMenu::kWidth, VrMenu::kImageHeight);
        menu.toast_key = key;
    }
    if (!menu.toast_shown) {
        menu.toast_shown = true;
        OpenXRSetCompileToastShown(true);
        PORTVR_LOG() << "shader compilation toast shown: " << done << " / " << total << std::endl;
    }
}

// What the menu shows besides its own state and the settings.
VrMenu::View BuildView(Menu& menu, const PortVrSettings& settings, double now) {
    if (now >= menu.readouts_at) {
        menu.readouts_at = now + 0.5;
        const OpenXRFrameTiming timing = OpenXRGetFrameTiming();
        menu.readouts.headset_hz = timing.headset_hz;
        menu.readouts.headset_fps = timing.rendered_fps;
        menu.readouts.game_fps = menu.rate.fps;
        const AuroraStats* stats = aurora_get_stats();
        menu.readouts.draws = stats != nullptr ? stats->drawCallCount : 0;
    }
    VrMenu::View view = menu.readouts;
    view.state_slot = menu.actions.StateSlot();
#if defined(__ANDROID__)
    view.standalone = true;
#endif
    view.foveation_live = aurora_stereo_foveation_available();
    view.multiview_available = aurora_get_stereo_multiview_available();
    view.in_game = PortDebug::StateManager() != nullptr;
    view.invulnerable = PortDebug::Invulnerable();
    view.streamed_audio = PortDebug::AiAudioEnabled();
    view.musyx_audio = PortDebug::MusyxAudioEnabled();
    // The setting: a log started for this run by MP_LOG_FILE goes on until it ends.
    view.log_file = PortDebug::LogFile();
    view.draw_tags = PortDebug::DrawTags();
    view.quick_resume = PortDebug::QuickResume();
    // The eye size the chosen render scale gives, at once when it changes.
    const OpenXREyeResolution eye = OpenXRGetEyeResolution(settings.render_scale);
    view.eye_width = eye.scaled_width;
    view.eye_height = eye.scaled_height;
    return view;
}

Menu& TheMenu() {
    static Menu menu;
    return menu;
}

} // namespace
#endif

void VrMenuUpdate() noexcept {
#if defined(MP_ENABLE_OPENXR)
    Menu& menu = TheMenu();
    const double now = NowSeconds();
    // Every game frame, so the rate is current when the menu opens.
    menu.rate.Tick(now);
    // Every game frame too, as the PC's toast is, so the startup compile ends when its queue
    // first drains even while the menu is open.
    uint32_t compiled = 0;
    uint32_t toCompile = 0;
    const bool compiling = PortDebug::ShaderCompilationProgress(compiled, toCompile) && OpenXRIsRunning();
    if (!OpenXRIsRunning() || !OpenXRSettingsPanelOpen()) {
        if (menu.was_open) {
            menu.was_open = false;
            VrMenu::Close(menu.state);
            menu.shown.clear();
            aurora_set_stereo_panel_image(nullptr, 0, 0);
        }
        UpdateCompileToast(menu, compiling, compiled, toCompile);
        return;
    }
    // The menu takes the panel's image over.
    UpdateCompileToast(menu, false, 0, 0);
    if (!menu.was_open) {
        menu.was_open = true;
        VrMenu::Open(menu.state);
    }

    const OpenXRSettingsPanelPointer pointer = OpenXRTakeSettingsPanelPointer();
    if (pointer.clicks > 0 && pointer.click_valid) {
        PortDebug::PollSettingsFile(true);
    }
    PortVrSettings settings = GetVrSettings();
    VrMenu::View view = BuildView(menu, settings, now);
    VrMenu::Refresh(menu.state, now);
    if (pointer.valid) {
        VrMenu::Hover(menu.state, view, pointer.y);
    }
    if (pointer.clicks > 0 && pointer.click_valid) {
        // Publish changed settings immediately so the desktop reflects the VR
        // menu without Save Settings or exit. Navigation alone does not write.
        VrMenu::ClickAndSave(menu.state, settings, view, pointer.click_x, pointer.click_y, now,
                            menu.actions, [](const PortVrSettings& changed) { SetVrSettings(changed); });
        menu.actions.Flush();
        // What the click changed outside the settings (a cheat, a switch, the slot).
        view = BuildView(menu, settings, now);
    }

    const bool notice = now < menu.state.saved_notice_until;
    std::string key = VrMenu::ViewKey(menu.state, settings, view, notice);
    if (key != menu.shown) {
        menu.shown = std::move(key);
        const VrMenu::Pixels pixels = VrMenu::BuildPixels(menu.state, settings, view, notice);
        aurora_set_stereo_panel_image(pixels.data(), VrMenu::kWidth, VrMenu::kImageHeight);
    }
#endif
}

} // namespace PortVr

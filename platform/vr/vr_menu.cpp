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
#include "port_discord.h"
#include "port_log_file.h"
#include "port_paths.h"
#include "port_savestate.h"
#include <ctime>
#include "port_textures.h"
#include "port_tracker.h"
#include "vr/openxr_integration.h"
#include "vr/openxr_settings_panel.h"

#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CGameState.hpp"

#include <aurora/aurora.h>
#include <aurora/gfx.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#endif

namespace PortVr {

#if defined(MP_ENABLE_OPENXR)
namespace {

double NowSeconds() noexcept {
    using namespace std::chrono;
    return duration< double >(steady_clock::now().time_since_epoch()).count();
}

CWorld* CurrentWorld() {
    CStateManager* mgr = PortDebug::StateManager();
    return mgr != nullptr ? mgr->World() : nullptr;
}

// The F1 overlay's goToArea (debug_ui/cheats.cpp): a loaded room (the current one and
// its neighbours) is entered in place; any other reloads the world straight into it.
void GoToArea(CWorld& world, int area) {
    if (area < 0 || area >= world.IGetAreaCount()) {
        return;
    }
    if (world.GetArea(TAreaId(area))->IsPostConstructed() || gpGameState == nullptr) {
        PortDebug::RequestTeleport(area);
    } else {
        PortDebug::RequestWorldTeleport(static_cast< uint32_t >(gpGameState->CurrentWorldAssetId()),
                                        static_cast< uint32_t >(world.IGetAreaAlways(TAreaId(area))->IGetAreaAssetId()));
    }
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

    // DEBUG's TELEPORT pages: the F1 overlay's Teleport block and its requests. The
    // overlay has no previous/next buttons here (Echoes' has); these step through the
    // same area and world tables its pickers list.
    void CycleArea(int direction) override {
        if (CWorld* world = CurrentWorld()) {
            const int count = world->IGetAreaCount();
            if (count > 0) {
                GoToArea(*world, ((world->GetCurrentAreaId().Value() + direction) % count + count) % count);
            }
        }
    }
    void CycleWorld(int direction) override {
        if (gpMemoryCard == nullptr || gpGameState == nullptr) {
            return;
        }
        const rstl::vector< CMemoryCard::MemoryWorld >& worlds = gpMemoryCard->GetMemoryWorlds();
        const int count = worlds.size();
        if (count == 0) {
            return;
        }
        int index = 0;
        for (int i = 0; i < count; ++i) {
            if (static_cast< uint32_t >(worlds[i].first) == static_cast< uint32_t >(gpGameState->CurrentWorldAssetId())) {
                index = i;
            }
        }
        index = ((index + direction) % count + count) % count;
        PortDebug::RequestWorldTeleport(static_cast< uint32_t >(worlds[index].first), 0u);
    }
    void CrossDimension() override {} // Prime 1 has one; the row is not shown
    void ReloadRoom() override {
        if (CWorld* world = CurrentWorld()) {
            GoToArea(*world, world->GetCurrentAreaId().Value());
        }
    }
    void GoToRoom(uint32_t area) override {
        if (CWorld* world = CurrentWorld()) {
            GoToArea(*world, static_cast< int >(area));
        }
    }
    void GoToWorld(uint32_t world) override { PortDebug::RequestWorldTeleport(world, 0u); }

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
    // Resume questions take priority over compilation progress while the menu is closed.
    enum class Toast { None, Compile, Resume };
    Toast toast = Toast::None;
    uint32_t toast_key = 0;
};

const char* ToastName(Menu::Toast toast) noexcept {
    return toast == Menu::Toast::Resume ? "resume question" : "shader compilation toast";
}

// The panel layer while the menu is closed (the menu has it while it is open): the boot's
// resume question, which holds the game until the pad answers it, else the startup compile's
// progress (PortDebug::ShaderCompilationProgress). The image is rebuilt only when what it shows
// changes, for the compile when its percent does.
void UpdateToast(Menu& menu, bool compiling, uint32_t done, uint32_t total) noexcept {
    const bool menu_open = OpenXRSettingsPanelOpen();
    PortSaveState::ResumeInfo question;
    bool yesSelected = true;
    const bool asking = OpenXRIsRunning() && PortSaveState::ResumePromptShown(question, yesSelected);
    std::string saved;
    if (asking) {
        const std::time_t timestamp = static_cast<std::time_t>(question.savedAt);
        std::tm local{};
#if defined(_WIN32)
        const bool dated = localtime_s(&local, &timestamp) == 0;
#else
        const bool dated = localtime_r(&timestamp, &local) != nullptr;
#endif
        if (dated) {
            char when[32]{};
            std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &local);
            saved = when;
        }
    }
    const Menu::Toast toast = menu_open ? Menu::Toast::None
                              : asking  ? Menu::Toast::Resume
                              : compiling ? Menu::Toast::Compile
                                          : Menu::Toast::None;
    if (toast != menu.toast && menu.toast != Menu::Toast::None) {
        PORTVR_LOG() << ToastName(menu.toast) << " hidden" << (menu_open ? " (VR menu open)" : "") << std::endl;
    }
    if (toast == Menu::Toast::None) {
        if (menu.toast != Menu::Toast::None) {
            menu.toast = Menu::Toast::None;
            OpenXRSetCompileToastShown(false);
            if (!menu_open) {
                aurora_set_stereo_panel_image(nullptr, 0, 0);
            }
        }
        return;
    }
    const uint32_t key = toast == Menu::Toast::Resume
                             ? (question.afterCrash ? 1u : 0u) | (question.crashedAgain ? 2u : 0u)
                             : VrMenu::CompileToastKey(done, total);
    if (toast != menu.toast || key != menu.toast_key) {
        const VrMenu::Pixels pixels =
            toast == Menu::Toast::Resume
                ? VrMenu::BuildResumeToastPixels(question.room, saved, question.afterCrash,
                                                 question.crashedAgain)
                : VrMenu::BuildCompileToastPixels(done, total);
        aurora_set_stereo_panel_image(pixels.data(), VrMenu::kWidth, VrMenu::kImageHeight);
        menu.toast_key = key;
    }
    if (toast != menu.toast) {
        if (menu.toast == Menu::Toast::None) {
            OpenXRSetCompileToastShown(true);
        }
        menu.toast = toast;
        if (toast == Menu::Toast::Resume) {
            PORTVR_LOG() << "resume question shown: " << question.room << (question.afterCrash ? " (after a crash)" : "")
                         << std::endl;
        } else {
            PORTVR_LOG() << "shader compilation toast shown: " << done << " / " << total << std::endl;
        }
    }
}


// DEBUG's TELEPORT lists, as the F1 overlay's pickers name them: the rooms by area index
// (PortTracker's names, which load over a few calls) and the card's world table.
void AddTeleportPlaces(VrMenu::View& view) {
    CWorld* world = CurrentWorld();
    if (world == nullptr) {
        return;
    }
    view.area = world->GetCurrentAreaId().Value();
    view.world = gpGameState != nullptr ? static_cast< uint32_t >(gpGameState->CurrentWorldAssetId()) : 0;
    const std::vector< std::string >& names = PortTracker::RoomNames(*world);
    const int count = world->IGetAreaCount();
    view.rooms.reserve(static_cast< size_t >(count));
    for (int i = 0; i < count; ++i) {
        char label[160];
        if (i < static_cast< int >(names.size()) && !names[i].empty()) {
            std::snprintf(label, sizeof(label), "%d  %s", i, names[i].c_str());
        } else {
            std::snprintf(label, sizeof(label), "Area %d", i);
        }
        view.rooms.push_back({static_cast< uint32_t >(i), label});
    }
    if (gpMemoryCard != nullptr) {
        const rstl::vector< CMemoryCard::MemoryWorld >& worlds = gpMemoryCard->GetMemoryWorlds();
        for (int i = 0; i < worlds.size(); ++i) {
            std::string name = PortDiscord::GameTextToUtf8(worlds[i].second.GetFrontEndName());
            if (!name.empty()) {
                view.worlds.push_back({static_cast< uint32_t >(worlds[i].first), std::move(name)});
            }
        }
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
    // Only DEBUG shows the lists, and they are a few dozen names to copy.
    if (menu.state.tab == VrMenu::kDebugTab) {
        AddTeleportPlaces(view);
    }
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
        UpdateToast(menu, compiling, compiled, toCompile);
        return;
    }
    // The menu takes the panel's image over.
    UpdateToast(menu, false, 0, 0);
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

// F1 overlay: The gallery window. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_gyro_motion.h"
#include "port_paths.h"
#include "port_gci.h"
#include "port_discord.h"
#include "port_gallery.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <fstream>
#include <vector>

namespace PortDebug {

// The Remastered gallery (port_gallery.h): the concept art the Remastered import put in the mods, shown one picture at
// a time in a window of its own. Only the current picture is decoded.
bool sGalleryOpen = false;
std::vector<std::string> sGalleryPaths;
int sGalleryIndex = 0;

namespace {
int sGalleryLoaded = -1;      // the picture sGalleryTexture holds
ImTextureID sGalleryTexture = 0;
ImVec2 sGallerySize{0.0f, 0.0f};
bool sGalleryFailed = false;
// Textures no longer shown, freed once the frames that drew them are done with.
struct RetiredTexture {
  ImTextureID texture;
  int frames;
};
std::vector<RetiredTexture> sGalleryRetired;
} // namespace

void ReleaseGalleryTexture() {
  if (sGalleryTexture != 0) {
    sGalleryRetired.push_back({sGalleryTexture, 0});
    sGalleryTexture = 0;
  }
  sGalleryLoaded = -1;
}

void FreeRetiredGalleryTextures(bool all) {
  for (size_t i = 0; i < sGalleryRetired.size();) {
    // Two frames: the one that drew it may still be on its way to the GPU.
    if (all || ++sGalleryRetired[i].frames > 2) {
      aurora_imgui_remove_texture(sGalleryRetired[i].texture);
      sGalleryRetired.erase(sGalleryRetired.begin() + i);
    } else {
      ++i;
    }
  }
}

void CloseGallery() {
  sGalleryOpen = false;
  // Retired, not freed: this frame may already have drawn it. DrawUI frees it a few frames on.
  ReleaseGalleryTexture();
  sGalleryPaths.clear();
}

namespace {
void LoadGalleryPicture() {
  ReleaseGalleryTexture();
  sGalleryFailed = true;
  sGalleryLoaded = sGalleryIndex;
  std::ifstream file(PortGci::PathFromString(sGalleryPaths[sGalleryIndex]), std::ios::binary);
  const std::vector<uint8_t> jpeg = port::ReadAll(file);
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
  if (!PortGallery::DecodeGalleryJpeg(jpeg.data(), jpeg.size(), width, height, rgba)) {
    return;
  }
  sGalleryTexture = aurora_imgui_add_texture(uint32_t(width), uint32_t(height), rgba.data());
  sGallerySize = ImVec2(float(width), float(height));
  sGalleryFailed = false;
}
} // namespace

void DrawGalleryWindow() {
  FreeRetiredGalleryTextures(false);
  if (!sGalleryOpen) {
    return;
  }
  const int count = int(sGalleryPaths.size());
  const ImVec2 display = ImGui::GetMainViewport()->Size;
  ImGui::SetNextWindowSize(ImVec2(display.x * 0.8f, display.y * 0.8f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (ImGui::Begin("Gallery##port", &open)) {
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    int step = 0;
    if (ImGui::Button("Previous") || (focused && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))) {
      step = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Next") || (focused && ImGui::IsKeyPressed(ImGuiKey_RightArrow))) {
      step = 1;
    }
    ImGui::SameLine();
    ImGui::Text("%d / %d", sGalleryIndex + 1, count);
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      open = false;
    }
    if (step != 0) {
      sGalleryIndex = (sGalleryIndex + step + count) % count;
    }
    if (sGalleryLoaded != sGalleryIndex) {
      LoadGalleryPicture();
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (sGalleryFailed) {
      ImGui::TextDisabled("Cannot read this picture.");
    } else if (avail.x > 1.0f && avail.y > 1.0f) {
      const float scale = std::min(avail.x / sGallerySize.x, avail.y / sGallerySize.y);
      const ImVec2 shown(sGallerySize.x * scale, sGallerySize.y * scale);
      const ImVec2 cursor = ImGui::GetCursorPos();
      ImGui::SetCursorPos(ImVec2(cursor.x + (avail.x - shown.x) * 0.5f, cursor.y + (avail.y - shown.y) * 0.5f));
      // The import writes the TXTR rows as stored, bottom row first, so draw them flipped.
      ImGui::Image(sGalleryTexture, shown, ImVec2(0.f, 1.f), ImVec2(1.f, 0.f));
    }
  }
  ImGui::End();
  if (!open) {
    CloseGallery();
  }
}

} // namespace PortDebug

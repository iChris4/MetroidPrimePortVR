#pragma once

#include <array>
#include <cstdint>

// Shipped Intro_Reactor graph, not a positional/model-name match. The five retail
// platforms start coincident; their distinct Play/Next waypoint links establish
// correspondence. Each Remastered platform links both effects, and two type-19
// SpecialFunctions copy its complete transform to them. Import must verify those
// links before using this table. GUID strings use canonical, not bytes_le order.
namespace PortQueenBarrier {
constexpr uint32_t kMrea = 0x87452DC1;
constexpr uint32_t kController = 0x00190066;
constexpr uint32_t kRetailModel = 0x6556EEC5;
constexpr const char* kControllerGuid = "2750d2f7-7c1a-4902-8a25-1dc2a530d5e7";
constexpr uint32_t kRefractionFeature = 1u << 29;
constexpr uint32_t kGlowFeature = 1u << 30;

struct AssetRing {
  uint32_t platform, waypoint;
  const char* component;
  const char* refraction;
  const char* glow;
  const char* breakupRefraction;
  const char* motion;
  const char* breakupGlow;
};

constexpr std::array<AssetRing, 5> kRings{{
    {0x0019006D, 0x001900BA, "bde384d0-25ca-43e8-92b4-4df120bb86a2",
     "2bd94e37-b30f-4adf-aa28-5d84c90caa96", "08a24b7d-09fe-46e1-9e16-0b4245cb22cc",
     "b84afb0d-b18c-4adb-9032-eea36a9f58b6", "e51675c6-0f3e-45cd-a3ed-322fb321738d",
     "f5ffd62d-34e5-4efb-a381-1c6375d72f98"},
    {0x0019006E, 0x001900BC, "fc2cdde9-6c09-4a98-aaa1-c92c8bde01e4",
     "a4cc85e7-2af1-4a03-b150-dc100ba18ec9", "6178c475-34ba-4b4e-a8a2-726112ff0df4",
     "59415cc5-934c-413c-aebc-79a323ba3975", "cbf943e9-9e2f-40fc-9640-512b4be42a16",
     "2291d91d-dba7-4c92-8c11-901b2527d670"},
    {0x00190077, 0x00190071, "1ccdcd85-b81b-4e50-836f-cd7c676d179c",
     "843f814b-ee2c-41ce-bbd9-f9b74072d666", "c9b07c07-fd61-4338-9d01-4a492c073908",
     "448ff2b0-9a57-452c-b459-2fef0faa32d1", "57a2609a-b8e9-4e2a-a49a-d5d7f38f8221",
     "af81b57f-d34e-4491-9640-a696415fd0d0"},
    {0x00190078, 0x001900B9, "980f34e8-392a-4f61-b162-138289fe6f16",
     "82f20986-8244-4e97-b60d-6d27c4ac655a", "85b360b3-f7ae-4928-9760-f1c2cd8a1252",
     "8a5760f2-6281-43fd-9de4-17938bd15f69", "4a6d74b8-e74e-43c2-adc1-be9b77bc8b69",
     "8f406f32-d966-4189-8e17-72e34d1350a7"},
    {0x00190079, 0x00190072, "d07ff4a7-1850-4f54-9673-a40820641563",
     "98392617-16ca-436e-895c-f667699c50c6", "f078ee44-f642-4028-bb93-e07a642de545",
     "f3d91a2c-8428-42eb-929a-a77f8b1c6204", "ecee5fc0-c18e-4009-a178-afea73a88ad8",
     "176578ff-f5a4-4b5f-967e-59bf0426ddb2"},
}};
} // namespace PortQueenBarrier

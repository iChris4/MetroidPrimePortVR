#include <gtest/gtest.h>

#include "gfx/frame_packet.hpp"
#include "gfx/clear.hpp"
#include "gfx/recording.hpp"
#include "gfx/texture.hpp"
#include "gfx/resources.hpp"
#include "gfx/stereo_uniform.hpp"
#include "gx/shader_info.hpp"
#include "gx/pipeline.hpp"
#include "webgpu/gpu.hpp"

#include <algorithm>
#include <array>
#include <memory>

namespace aurora::gfx {
namespace {

constexpr auto ColorFormat = wgpu::TextureFormat::RGBA8Unorm;
constexpr auto DepthFormat = wgpu::TextureFormat::Depth24Plus;

class GfxRecordingTest : public ::testing::Test {
protected:
  void SetUp() override {
    webgpu::g_graphicsConfig.surfaceConfiguration.format = ColorFormat;
    webgpu::g_graphicsConfig.depthFormat = DepthFormat;
    webgpu::g_graphicsConfig.msaaSamples = 1;
    webgpu::g_frameBuffer.size = {640, 480, 1};
    webgpu::g_frameBuffer.format = ColorFormat;
    webgpu::g_depthBuffer.size = {640, 480, 1};
    webgpu::g_depthBuffer.format = DepthFormat;
    detail::testing::suppress_render_worker(true);
    detail::begin_recording(frame, 0);
  }

  void TearDown() override {
    if (recordingActive) {
      if (is_offscreen()) {
        end_offscreen();
      }
      finish();
      detail::end_recording();
    }
    detail::shutdown_recording();
  }

  void seed(uint32_t width, uint32_t height) {
    detail::testing::seed_offscreen_cache(width, height, ColorFormat, DepthFormat);
  }

  void copy_current_offscreen() {
    const auto& pass = frame.renderPasses.back();
    const auto& size = pass.colorAttachments[SceneColorAttachmentIndex].size;
    auto target = std::make_shared<TextureRef>(wgpu::Texture{}, wgpu::TextureView{}, wgpu::TextureView{}, size,
                                               ColorFormat, 1, GX_TF_RGBA8);
    resolve_pass_into(std::move(target), {0, 0, static_cast<int32_t>(size.width), static_cast<int32_t>(size.height)},
                      false, false, false, {}, 1.f);
  }

  size_t count_efb_passes() const {
    return static_cast<size_t>(
        std::ranges::count_if(frame.renderPasses, [](const auto& pass) { return pass.label.starts_with("EFB"); }));
  }

  detail::FramePacket frame;
  bool recordingActive = true;
};

gx::PipelineConfig MapBatchConfig() {
  gx::g_gxState = {};
  auto& state = gx::g_gxState;
  state.mapBatch = true;
  state.cullMode = GX_CULL_FRONT;
  state.deindexVertices = true;
  for (auto attr : {GX_VA_POS, GX_VA_NRM, GX_VA_CLR0, GX_VA_TEX0}) {
    state.vtxDesc[attr] = GX_DIRECT;
  }
  auto& vat = state.vtxFmts[7].attrs;
  vat[GX_VA_POS] = {GX_POS_XYZ, GX_F32, 0};
  vat[GX_VA_NRM] = {GX_NRM_XYZ, GX_F32, 0};
  vat[GX_VA_CLR0] = {GX_CLR_RGBA, GX_RGBA8, 0};
  vat[GX_VA_TEX0] = {GX_TEX_ST, GX_F32, 0};
  state.numTevStages = 1;
  state.tevStages[0].channelId = GX_COLOR0A0;
  state.tevStages[0].colorPass.d = GX_CC_RASC;
  state.tevStages[0].alphaPass.d = GX_CA_RASA;
  for (auto& channel : state.colorChannelConfig) { channel.matSrc = GX_SRC_VTX; }
  gx::PipelineConfig config{};
  gx::populate_pipeline_config(config, GX_TRIANGLES, GX_VTXFMT7);
  return config;
}

TEST_F(GfxRecordingTest, MapBatchLayoutAndCullStateAreIsolated) {
  auto config = MapBatchConfig();
  EXPECT_TRUE(config.shaderConfig.mapBatch);
  EXPECT_EQ(config.cullMode, GX_CULL_NONE);
  EXPECT_EQ(config.shaderConfig.mapCull, GX_CULL_FRONT);
  EXPECT_EQ(config.shaderConfig.vtxStride, 36);
  EXPECT_EQ(config.shaderConfig.attrs[GX_VA_NRM].offset, 12);
  EXPECT_EQ(config.shaderConfig.attrs[GX_VA_CLR0].offset, 24);
  EXPECT_EQ(config.shaderConfig.attrs[GX_VA_TEX0].offset, 28);
  gx::g_gxState.tevSwapTable[0].red = GX_CH_BLUE;
  gx::populate_pipeline_config(config, GX_TRIANGLES, GX_VTXFMT7);
  EXPECT_EQ(config.shaderConfig.tevSwapTable[0].red, GX_CH_RED);
  EXPECT_EQ(gx::g_gxState.tevSwapTable[0].red, GX_CH_BLUE);
  gx::g_gxState.mapBatch = false;
  gx::populate_pipeline_config(config, GX_TRIANGLES, GX_VTXFMT7);
  EXPECT_EQ(config.cullMode, GX_CULL_FRONT);
  EXPECT_FALSE(config.shaderConfig.mapBatch);
  EXPECT_EQ(config.shaderConfig.mapCull, GX_CULL_NONE);
}

TEST_F(GfxRecordingTest, MapBatchShaderExpandsOutlinesInEveryEyeMode) {
  auto config = MapBatchConfig().shaderConfig;
  for (auto mode : {gx::MultiviewNone, gx::MultiviewClip, gx::MultiviewFull, gx::EyeClipImmediate}) {
    config.multiview = mode;
    const auto source = gx::build_shader_source(config);
    EXPECT_NE(source.find("let in_tex0_uv ="), std::string::npos);
    EXPECT_NE(source.find("in_tex0_uv.x * min(viewport_scale"), std::string::npos);
    EXPECT_NE(source.find("@builtin(front_facing) map_front: bool"), std::string::npos);
    EXPECT_NE(source.find("if (in.map_fill != 0u && map_front) { discard; }"), std::string::npos);
    EXPECT_EQ(source.find("in_tex0."), std::string::npos);
    if (mode == gx::MultiviewClip || mode == gx::EyeClipImmediate) {
      EXPECT_NE(source.find("vec4f(mv_pos_a, 1.0) * ubuf_mv.eye_clip0"), std::string::npos);
      EXPECT_NE(source.find("vec4f(mv_pos_b, 1.0) * ubuf_mv.eye_clip1"), std::string::npos);
      EXPECT_EQ(source.find("* ubuf.proj"), std::string::npos);
    }
    if (mode == gx::MultiviewClip || mode == gx::MultiviewFull) {
      EXPECT_NE(source.find("@builtin(view_index) mv_view_in"), std::string::npos);
    } else {
      EXPECT_EQ(source.find("@builtin(view_index)"), std::string::npos);
    }
  }
}

TEST_F(GfxRecordingTest, MapBatchColoursAndWidthsDoNotSplitUniforms) {
  const auto config = MapBatchConfig().shaderConfig;
  const auto info = gx::build_shader_info(config);
  EXPECT_FALSE(info.sampledKColors.any());
  EXPECT_FALSE(info.sampledTextures.any());
  EXPECT_EQ(info.lineMode, 0);
  detail::resources().limits.minUniformBufferOffsetAlignment = 256;
  ByteBuffer snapshot;
  std::array<uint32_t, 2> offsets{};
  gx::build_uniform(info, offsets, &snapshot);
  gx::g_gxState.lineWidth += 6;
  gx::g_gxState.kcolors[0][0] += 1.f;
  EXPECT_TRUE(gx::uniform_matches(info, snapshot));
}

TEST_F(GfxRecordingTest, CreateRestoreReturnsToEfb) {
  seed(320, 180);
  begin_offscreen(320, 180);
  ASSERT_TRUE(is_offscreen());

  end_offscreen();

  EXPECT_FALSE(is_offscreen());
  ASSERT_EQ(frame.renderPasses.size(), 2u);
  EXPECT_TRUE(frame.renderPasses[0].sealed);
  EXPECT_TRUE(frame.renderPasses[0].discardable);
  EXPECT_EQ(count_efb_passes(), 1u);
}

TEST_F(GfxRecordingTest, UniformSnapshotComparesConsumedValues) {
  gx::g_gxState = {};
  detail::resources().limits.minUniformBufferOffsetAlignment = 256;
  const auto info = gx::build_shader_info(gx::ShaderConfig{});
  ByteBuffer snapshot;
  std::array<uint32_t, 2> offsets{};
  gx::build_uniform(info, offsets, &snapshot);
  ASSERT_TRUE(gx::uniform_matches(info, snapshot));
  // Triangle shaders do not consume line width or an unused konst register.
  ++gx::g_gxState.lineWidth;
  gx::g_gxState.kcolors[0][0] += 1.0f;
  EXPECT_TRUE(gx::uniform_matches(info, snapshot));
  gx::g_gxState.pnMtx[0].pos.m0[0] += 1.0f;
  EXPECT_FALSE(gx::uniform_matches(info, snapshot));
}

TEST_F(GfxRecordingTest, UniformSnapshotIncludesLogicalTextureSizeAndLineWidth) {
  gx::g_gxState = {};
  detail::resources().limits.minUniformBufferOffsetAlignment = 256;
  gx::ShaderConfig config{};
  config.lineMode = 1;
  auto info = gx::build_shader_info(config);
  info.sampledTextures.set(0);
  info.uniformSize += 16;
  ByteBuffer snapshot;
  std::array<uint32_t, 2> offsets{};
  gx::build_uniform(info, offsets, &snapshot);
  ASSERT_TRUE(gx::uniform_matches(info, snapshot));
  ++gx::g_gxState.lineWidth;
  EXPECT_FALSE(gx::uniform_matches(info, snapshot));
  --gx::g_gxState.lineWidth;
  gx::g_gxState.textures[0].texObj.image0 ^= 1;
  EXPECT_FALSE(gx::uniform_matches(info, snapshot));
  EXPECT_FALSE(gx::uniform_matches(info, ByteBuffer{}));
}

TEST_F(GfxRecordingTest, PushPastMappedBufferSpillsAndHonoursFrameLimit) {
  std::array<uint8_t, 64> mapped{};
  frame.storage = ByteBuffer{mapped.data(), mapped.size()};
  detail::resources().limits.minStorageBufferOffsetAlignment = 16;
  std::array<uint8_t, 48> data{};
  data.fill(37);
  EXPECT_FALSE(overflowed(push_storage(data.data(), data.size())));
  const auto second = push_storage(data.data(), data.size());
  EXPECT_FALSE(overflowed(second));
  EXPECT_EQ(second.offset, frame_buffer_sizes().base(StagingKind::Storage) + mapped.size());
  EXPECT_EQ(frame.storage.size(), mapped.size());
  ASSERT_EQ(frame.spillStorage.length, data.size());
  EXPECT_EQ(std::memcmp(frame.spillStorage.data.get(), data.data(), data.size()), 0);
  EXPECT_TRUE(overflowed(push_storage(data.data(), frame_buffer_sizes().maxStorage + 1)));
  EXPECT_EQ(frame.spillStorage.length, data.size());
  frame.storage = ByteBuffer{};
  frame.spillStorage = {};
}

TEST_F(GfxRecordingTest, MultiviewClipBlockRemainsAdjacentWhenUniformStagingSpills) {
  detail::resources().limits.minUniformBufferOffsetAlignment = 256;
  std::array<uint8_t, 1536> mono{};
  mono[0] = 42;
  std::array<uint8_t, 1536> mapped{};
  frame.uniforms = ByteBuffer{mapped.data(), mapped.size()};
  const auto range = push_uniform(mono.data(), mono.size());
  ASSERT_FALSE(overflowed(range));
  StereoFrameState state{};
  state.immersive = true;
  state.multiview = true;
  set_frame_stereo(state);
  set_stereo_draw_route(AURORA_STEREO_ROUTE_FULLSCREEN);
  const auto layout = StereoUniformLayout::for_gx(0, false, 0, mono.size());
  ASSERT_TRUE(layout.valid());
  const auto offsets = stage_stereo_uniforms(mapped.data(), range, layout);
  EXPECT_EQ(offsets[0], mapped.size());
  EXPECT_EQ(offsets[1], offsets[0]);
  EXPECT_EQ(stereo_multiview_mode(), gx::MultiviewClip);
  ASSERT_EQ(frame.spillUniforms.length, mono.size() + 144);
  EXPECT_EQ(std::memcmp(frame.spillUniforms.data.get(), mono.data(), mono.size()), 0);
  EXPECT_FALSE(frame.stereo.uniformsExhausted);
  frame.uniforms = ByteBuffer{};
  frame.spillUniforms = {};
}

TEST_F(GfxRecordingTest, EfbPassUsesDiscoveredSceneLayout) {
  ASSERT_FALSE(frame.renderPasses.empty());
  const auto discovered = scene_render_target_layout();
  const auto targetLayout = frame.renderPasses.front().target_layout();

  EXPECT_EQ(targetLayout.key, discovered.key);
  EXPECT_EQ(targetLayout.colorAttachmentCount, discovered.colorAttachmentCount);
  EXPECT_EQ(targetLayout.colorAttachments[SceneColorAttachmentIndex].semantic, ColorAttachmentSemantic::SceneColor);
}

TEST_F(GfxRecordingTest, CopiedOffscreenPassIsRetainedOnRestore) {
  seed(320, 180);
  begin_offscreen(320, 180);
  copy_current_offscreen();

  end_offscreen();

  ASSERT_EQ(frame.renderPasses.size(), 3u);
  EXPECT_TRUE(frame.renderPasses[0].sealed);
  EXPECT_FALSE(frame.renderPasses[0].discardable);
  EXPECT_TRUE(frame.renderPasses[0].has_consumer());
  EXPECT_TRUE(frame.renderPasses[1].sealed);
  EXPECT_TRUE(frame.renderPasses[1].discardable);
}

TEST_F(GfxRecordingTest, ReplacementRetainsCopiedPassesAndDiscardsContinuations) {
  seed(320, 180);
  seed(160, 90);
  begin_offscreen(320, 180);
  copy_current_offscreen();
  begin_offscreen(160, 90);
  copy_current_offscreen();

  end_offscreen();

  ASSERT_EQ(frame.renderPasses.size(), 5u);
  EXPECT_TRUE(frame.renderPasses[0].has_consumer());
  EXPECT_FALSE(frame.renderPasses[0].discardable);
  EXPECT_TRUE(frame.renderPasses[1].discardable);
  EXPECT_TRUE(frame.renderPasses[2].has_consumer());
  EXPECT_FALSE(frame.renderPasses[2].discardable);
  EXPECT_TRUE(frame.renderPasses[3].discardable);
  EXPECT_EQ(count_efb_passes(), 1u);
}

TEST_F(GfxRecordingTest, RepeatedUncopiedCreatesDiscardEarlierPasses) {
  seed(320, 180);
  seed(160, 90);
  begin_offscreen(320, 180);
  begin_offscreen(160, 90);
  end_offscreen();

  ASSERT_EQ(frame.renderPasses.size(), 3u);
  EXPECT_TRUE(frame.renderPasses[0].sealed);
  EXPECT_TRUE(frame.renderPasses[0].discardable);
  EXPECT_TRUE(frame.renderPasses[1].sealed);
  EXPECT_TRUE(frame.renderPasses[1].discardable);
  EXPECT_EQ(count_efb_passes(), 1u);
}

TEST_F(GfxRecordingTest, PublicCreatePassRejectsExistingOffscreenPass) {
  seed(320, 180);
  seed(160, 90);
  ASSERT_TRUE(create_pass(320, 180));

  EXPECT_FALSE(create_pass(160, 90));

  ResolvedTargets ignored;
  EXPECT_TRUE(resolve_pass({.color = false, .depth = false}, ignored));
}

TEST_F(GfxRecordingTest, FinalizedPassesAreSealedOrDeliberatelyDiscarded) {
  seed(320, 180);
  seed(160, 90);
  begin_offscreen(320, 180);
  copy_current_offscreen();
  begin_offscreen(160, 90);
  end_offscreen();
  finish();

  ASSERT_FALSE(frame.renderPasses.empty());
  for (const auto& pass : frame.renderPasses) {
    EXPECT_TRUE(pass.sealed);
    if (!pass.has_consumer() && pass.label.starts_with("Offscreen")) {
      EXPECT_TRUE(pass.discardable);
    }
  }
  detail::end_recording();
  recordingActive = false;
}

} // namespace
} // namespace aurora::gfx

TEST(StereoClearRect, ScalesPartialClearToEyeResolution) {
  aurora::gfx::clear::DrawData data{};
  data.rect = {160, 120, 320, 240};
  data.rectReferenceWidth = 640;
  data.rectReferenceHeight = 480;
  const auto rect = aurora::gfx::clear::target_rect(data, wgpu::Extent3D{1280, 960, 1});
  EXPECT_EQ(rect.x, 320);
  EXPECT_EQ(rect.y, 240);
  EXPECT_EQ(rect.width, 640);
  EXPECT_EQ(rect.height, 480);
}

TEST(StereoClearRect, EmptyRectClearsTheWholeEye) {
  const auto rect = aurora::gfx::clear::target_rect({}, wgpu::Extent3D{2000, 2100, 1});
  EXPECT_EQ(rect.x, 0);
  EXPECT_EQ(rect.y, 0);
  EXPECT_EQ(rect.width, 2000);
  EXPECT_EQ(rect.height, 2100);
}

TEST(StereoFogCopy, DepthReadsEachEyesDepthAttachment) {
  std::array<aurora::gfx::detail::StereoEyePass, 2> eyes{};
  for (const auto& eye : eyes) {
    EXPECT_EQ(&eye.copy_view(true), &eye.depthView);
    EXPECT_NE(&eye.copy_view(true), &eye.colorView);
    EXPECT_NE(&eye.copy_view(true), &eye.copySourceView);
    EXPECT_EQ(&eye.copy_view(false), &eye.colorView);
  }
  EXPECT_NE(&eyes[0].copy_view(true), &eyes[1].copy_view(true));
}

TEST(StereoShaderLayout, UpstreamFlagsDoNotOverwriteEyeReplayFlags) {
  aurora::gx::ShaderConfig config{};
  config.currentPnMtx = 7;
  config.multiview = aurora::gx::EyeClipImmediate;
  config.mapBatch = 1;
  config.mapCull = 2;
  config.nativeVertices = 1;
  config.perPixelLight = 1;
  config.clipPlane = 1;
  config.shell = 2;
  config.pbrCut = 1;
  config.pbrThermal = 1;
  config.actorDecal = 1;
  EXPECT_EQ(config.currentPnMtx, 7);
  EXPECT_EQ(config.multiview, aurora::gx::EyeClipImmediate);
  EXPECT_EQ(config.mapBatch, 1);
  EXPECT_EQ(config.mapCull, 2);
  EXPECT_EQ(config.nativeVertices, 1);
  config.multiview = aurora::gx::MultiviewClip;
  EXPECT_EQ(config.perPixelLight, 1);
  EXPECT_EQ(config.clipPlane, 1);
  EXPECT_EQ(config.shell, 2);
  EXPECT_EQ(config.pbrCut, 1);
  EXPECT_EQ(config.pbrThermal, 1);
  EXPECT_EQ(config.actorDecal, 1);
  // tools/primedgun_seed.py must agree with the serialized row layout.
  EXPECT_EQ(sizeof(aurora::gx::PipelineConfig), 2788u);
}

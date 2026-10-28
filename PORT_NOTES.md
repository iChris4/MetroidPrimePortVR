## VR: upstream 0.35 local testing merge (2026-10-28)

Merged Odrannnn/MetroidPrimePort `port` at `011c131db` (298 incoming commits)
onto PrimedGun `5b60fce16`, on a local testing branch. Main and releases are
unchanged until the testing branch is explicitly approved for pushing.

Upstream now uses the old spare ShaderConfig bytes for clip planes, silhouette
shells and thermal flags. VR replay flags move to four explicit extra bytes,
with cache layout epoch 3. Old user GX cache rows are invalidated. The 3,173-row retail VR seed is
converted explicitly: old byte 10 moves to currentPnMtx at byte 12, old byte
11 bits 0-5 move to byte 13, and its lighting/decal bits move to upstream
bytes 10/11; the four-byte extension shifts attributes by four. Config version
16 and seed epoch 3 are validated. Shader compilation on the first test launch
can still be slower. Do not overlap these flags to preserve an old cache layout. Regression:
`StereoShaderLayout.UpstreamFlagsDoNotOverwriteEyeReplayFlags`.

Keep per-eye depth copies and full-eye retail fog bounds. Immersive VR retains
the existing pause/menu routing and skips the new mono inventory HDR/silhouette
composites; desktop pause lighting remains available. The new cannon depth
prepass is skipped in VR because it uses the desktop gun and exposure camera.
Remastered cannon hologram transitions use the tracked model transform. Native
vertex shadow entries are retained and rewritten to the correct eye uniform
binding; their regression checks all three stereo shader modes. Keep
the previous EnvFX and third-person guards, HMD minimap/billboards, roomscale,
left-hand model mirroring, cannon targeting and camera comfort behavior.

Windows testing uses upstream's new Dawn package rather than the old cached
SDK. This merge does not establish headset rendering correctness: compare
fog, beam switching, visors, pause inventory, HUD, morph transitions and
cinema direction in the headset before promoting it.

Windows build passed with Dawn v20261027.212404. Full CTest: 719 passed,
one skipped (entity schema generation requires the Linux clang-19 tool).
The per-eye fog, native-vertex stereo shadow, shader-layout separation and
VR menu/controller regression tests pass. Headset confirmation remains required.

## VR: upstream rendering merge checks (2026-10-25)

Upstream targets a desktop camera. A clean merge and passing desktop tests do
not establish correct stereo rendering. Audit changes to EFB copy formats,
depth attachments, copy uniforms, screen projection/crop rectangles, custom
draws and post-process encoder tasks against both per-eye replay and Quest
multiview. Preserve the HMD-aware camera, billboard and reticle paths.

The 0.32 merge changed retail fog copies from A8 to Z16. The stereo path
excluded depth formats, so both eyes sampled the mono depth copy. Depth
copies now get separate eye targets and use each eye's depth attachment;
VR fog also avoids crop bounds computed from the desktop camera. The user
confirmed the lava and normal fog correction in-headset. Regression tests:
`SceneHdrTest.RetailFogDepthCopiesHaveSeparateEyeTargets` and
`StereoFogCopy.DepthReadsEachEyesDepthAttachment`.

Do not restore colour-only copy gates or assume a new native custom draw
replays into the eyes. The new Remastered EnvFX weather draw is mono-only,
so immersive VR retains the tracked retail GX weather renderer until EnvFX
has a stereo implementation. The merge also preserves partial-clear scaling
for eye resolution and keeps mono HDR compositing/desktop third-person out
of the immersive view. Those protections need review on future merges too.

## VR: pipelined rendering on the Quest (2026-10-22)

`vr_pipelined_rendering` has been on by default on Android since 5e3ebaa87,
but it did nothing there: `PipelinedCycle` only runs on a backend whose
`SupportsPipelining()` is true, and only the D3D12 one was. The Quest's direct
presentation backend now implements the same contract (`openxr_d3d12.h`): the
next packet is located and published as soon as the game picks up this one,
so the game records frame N+1 while Aurora encodes N, for one frame of
latency. The AHardwareBuffer bridge (Direct to headset off) and PC Vulkan
still run render-first.

Quest 3, Chozo Ruins plaza (save state slot 1, 702 draws), render scale 0.85,
multiview, `MP_FRAME_STATS=1` in every run, 28 s of the game's VrApi lines:

| Refresh | Render-first | Pipelined |
|---|---|---|
| 72 Hz | not measured | 72 FPS, 0 stale/s, GPU 4.8 ms |
| 90 Hz | 74 FPS, 17 stale/s, GPU 5.8 ms | 90 FPS, 0 stale/s, GPU 5.2 ms |
| 120 Hz | 71 FPS, 50 stale/s, GPU 5.0 ms | 120 FPS, 4 stale/s, GPU 5.1 ms |

- `platform/vr/openxr_vulkan_direct.cpp`: D3D12's packet states (pending, one
  queued behind it on the retained swapchain pair, active), two submission
  results by token, `LocatePacket` / `ArmPacket`, and `EyesResizing`. Where it
  differs from D3D12:
  - Each packet carries the session run and reference-space change its poses
    were located in (`PoseOrigin`). D3D12 still overwrites
    `render_session_serial_` at `LocatePacket(N+1)`, before `FinishFrame(N)`
    reads it.
  - Retired eye pairs are reaped in `ArmPacket` behind a submitted packet, the
    only moment of a pipelined cycle when Aurora holds no targets
    (`aurora_vulkan_direct_forget_targets` refuses otherwise). D3D12 never
    reaps while pipelined.
  - `SupportsPipelining` needs two images or more per swapchain: a queued
    packet's image comes from the pair on display.
  - A session that stops with a packet queued waits up to 100 ms for that
    packet's submission when it cannot be cancelled, instead of abandoning its
    images, which would also keep that pair from ever being resized.
- `extern/aurora/lib/webgpu/vulkan_direct_interop.cpp`: `Encode` waits up to
  20 ms for its packet's targets, as the D3D12 bridge does, since `ArmPacket`
  runs right after the previous packet's submission. A cancelled token skips
  the wait.

Checked on the Quest 3 at the desk (proximity automated): the log reads
`OpenXR Vulkan pacing: render-first, pipelined`; three save-state reloads at
90 Hz, the virtual screen and immersive switches around them, and a console
`quit` (the session stops with packets in flight, then Shutdown) ran with no
skipped eye copy, no unarmed packet, no fault, and flat memory. The desk could
not reach a render scale change from the VR menu while pipelined (the resize
and retire path), the VR menu's panel, or taking the headset off (`prox_far`
does not stop the session); the user then played the build in the headset
before the commit.

## VR: an empty multiview eye pass faults the Quest 3's GPU (2026-10-19)

beta.1 (upstream port 0.30.0) crashed on the Quest 3 with Multiview eyes on: a
few frames after the presentation turned immersive after a save load, a resume,
a save-state load from the VR menu or the Artifact Temple's crater warp, the
kernel logged "adreno-gen7-gmu CP opcode error", Dawn "vkQueueSubmit failed with
VK_ERROR_DEVICE_LOST", and the render worker aborted. Multiview off never
crashed; Dawn's validation sees nothing wrong.

The cause, from the kernel log against a per-pass count of the draws each eye
pass issues: every Vulkan multiview render pass that issues no draw at all is
one CP opcode error on the Adreno 740 (deterministic: four empty passes, four
faults), and kgsl kills the context after three faults within two seconds,
which Dawn reports as the device lost at its next submit. Passes with nothing
in them come from the frames a save state or world load records (four frames
of viewport and scissor commands only), and from upstream 570dc7709, which
builds every Android build's pipelines on first use instead of at startup
(issue #162): the first immersive frames then replay passes whose draws all
wait for their pipeline, so the HUD pass was empty for four frames and the
world pass had 205 of 374 draws. That is why beta.1 crashed on the first save
load while 0.28 and 0.29 (precompiled) only crashed on later reloads.

- `extern/aurora/lib/gfx/encoding.cpp`: `stereo_multiview_pass_draws` scans the
  pass for a GX draw with a built multiview pipeline and staged eye uniforms, or
  a clear with its multiview pipeline; when there is none, the pass replays per
  eye (`render_stereo_eye_pass`, whose views exist under multiview too), which
  honours the same clear and load semantics and faults nothing. Draws whose
  multiview pipeline is still building are skipped there as before.
- `platform/main.cpp`: the OpenXR build builds the cached pipelines at startup
  on Android again (6.4-7.0 s behind the title and the compile toast on a warm
  Quest 3 cache); only a low-RAM device (under 4 GiB or `isLowRamDevice()`)
  builds them on first use. `MP_PIPELINE_LAZY=1` still forces first use.
- `extern/aurora/lib/gx/command_processor.cpp`: the eye passes' multiview
  pipelines (MultiviewClip, MultiviewFull) are looked up blocking, so a config
  the cache lacks waits for its build (a one-off hitch; 260 ms with every
  multiview pipeline missing) instead of thinning a multiview pass. Per-eye
  passes (PC VR, multiview off) still skip a draw whose pipeline is building.

Desk reproduction on the Quest 3 (headset on the desk, proximity automated,
`MP_SKIP_CUTSCENES=1 MP_FAST_BOOT=1`, logs read back over adb): a save-state
load through the console (`state load 8`, Artifact Temple) faulted 2 of 2 times
before the change (3 empty passes, 4 faults each) and 0 of 3 after; the console
warp to the Crater Entry Point and 3 cold boots into the Landing Site ran with no
fault, and the kernel log showed none across the run. Before the precompile
change, the Landing Site boot alone faulted in about 7 of 10 boots. Ruled out
on the way: Dawn validation (clean), the storage clamp, passthrough, shadows,
room geometry on the GPU, deindexing, direct presentation, pipelined
rendering, the EFB resize rework, Dawn's recycled-memory release, the staging
spill, and serialising pipeline creation against Queue::Submit.

## The map screen draws its rooms in one batch (2026-10-12)

On the Quest 3 the map screen was the slowest screen in the game: one GX draw
per room surface, outline and door face, about 5,250 at the Chozo Ruins plaza.
On the virtual screen it ran at 36 FPS of 90; the 3D map in the headset's view
(which draws the live world too) stalled at about 1 FPS with multiview. The HUD
minimap's ordered batch (`PortMapBatch`) now covers the map screen
(`MP_MAP_SCREEN_BATCH=0` keeps the old path), its records are kept across
frames and handed to the FIFO as one display list instead of four GX calls per
vertex. The virtual-screen map now holds 90 FPS (82 draws, 1.9 ms of GPU) and
the 3D map runs at gameplay's rate there (66 FPS, 710 draws). Measurements,
the profile and what remains in `docs/CHOZO_PERFORMANCE.md` ("Map screen
batch").

## VR: a render scale change no longer leaves the eyes swapping sizes (2026-10-12)

The user lowered `vr_render_scale` in game (PSVR2 on SteamVR, RTX 3090, eyes
4808x4904 at scale 1 with 4x MSAA) and the frame rate got worse, not better.
The log had ~5,000 pairs of "Stereo eye N replay target" lines alternating
between two sizes: Aurora made both eyes' targets again every frame.

- The backends' two eye swapchain pairs take turns (one shown, one written),
  and `ResizeWritablePair` rebuilds only the written one, only with no packet
  in flight. Pipelined pacing (D3D12 only) drained one cycle per new scale:
  one pair was rebuilt, the other kept its size until the pipeline next
  drained, and the packets alternated sizes every frame meanwhile. The
  integration now keeps the cycles plain while `EyesResizing()` (a pair not
  at the size aimed for, rebuilds still allowed).
- A slider's drag also ran into SteamVR's swapchain cap
  (`XR_ERROR_LIMIT_REACHED`, -10): each rebuild keeps the replaced pair alive
  for `kOpenXRRetiredSwapchainCycles`, and a refusal aims back at the refused
  pair's size, which the other pair may have to be rebuilt to again.
  `vr/openxr_eye_resize.h`'s `OpenXREyeResizeGate`, in all three backends:
  no rebuild while a retired pair lives (the session holds the two pairs in
  use plus the one being made), and after two refusals of one request no
  more until another. The packets still alternate for about eight frames per
  change while the first pair's old images are kept.
- Tests: `port_vr_eye_resize_tests` models the pairs, the retired delay, the
  runtime's cap and pipelined pacing (the old rules reproduce the endless
  swap; a change, a drag, a refused size and a refusal both ways settle).

## VR: the rumble split by source, cannon on the gun hand (2026-10-12)

The user asked for the charge shot to rumble only the gun hand's controller
while landings and damage rumble both. The VR side could not tell them apart:
`CRumbleGenerator` mixes every effect on port 0 into one pulsed motor, and the
PADControlMotor hook only saw on/off.

- `CRumbleManager::Rumble` is the only place that knows the effect. On
  TARGET_PC it goes through `PortRumble(..., EPortRumbleSource)`: while the VR
  pad owns port 0 (`PortVr::VrPadOwnsPort0`), the arm cannon's effects play on
  the generator's port 1 voices and the grapple arm's on port 2, each pulsed on
  its own; a pad, or the headset in Gamepad mode, gets everything on port 0 as
  before. Arm cannon: `kRFX_PlayerGunCharge`, `kRFX_PlayerMissileFire`, the
  Wave Buster's lock bump (`kRFX_PlayerBump`, named at its call site). Grapple
  arm: `kRFX_PlayerGrappleFire`, `kRFX_PlayerGrappleSwoosh`. Everything else is
  the body. The port rides in the handle's bits 4-5 (CRumbleVoice's handles are
  `owner << 8 | channel 0-3`), so `StopRumble` stops the effect where it plays.
- The recoil of charged shots, charged Plasma, Super Missiles, the Flamethrower
  and the Wave Buster rumbled through their camera shake
  (`CCameraManager::Update`: `kRFX_CameraShake`, gain 1, priority 2). The
  headset skips those shakes, so it had no rumble on them at all;
  `CRumbleManager::PortRecoilRumble` now plays that same pulse from the arm
  cannon where the shake is skipped.
- `vr_pad.cpp`'s motor hook (`ClaimRumbleMotors`, was `ClaimPortZeroMotor`)
  takes ports 0-2 and feeds three channels (`OpenXRSetRumble(amplitude,
  RumbleChannel)`; `OpenXRClearRumble` stops them all). Ports 1 and 2 are always
  claimed once the hook is in: the game rumbles no other port, so an effect
  left playing there when the controllers let go never reaches a real pad.
- `OpenXRInput::UpdateRumble` gives each hand `RumbleHandAmplitudes`
  (header-only `platform/include/vr/vr_rumble.h`, test `port_vr_rumble_tests`):
  new target `split` = body on both, cannon on the gun hand
  (`vr_use_right_hand`), grapple on the off hand, each hand the stronger of
  its two. `both` / `left` / `right` play the strongest channel on their
  hand(s), as before.
- `vr_rumble_hand=split` is the new default (`PortVrSettings`, launcher keys,
  the VR menu's Reset Controller). The VR menu cycles SPLIT -> BOTH -> LEFT ->
  RIGHT; the F1 VR tab has a Rumble target combo now; PC launcher and Quest
  panel list it first, with a note. A port_settings.ini that already holds
  `vr_rumble_hand` keeps its value. Importing a PrimedGun config still maps its
  rumble_hand_mode 0/1/2 to both/left/right.

## VR: the pause menu, logbook and map stay in the headset view (2026-10-12)

The user asked for the message boxes' immersive treatment (previous entry) on
the pause menu and the map screen too, through PrimedGun's existing "Detach
game menu and map from view" (`vr_game_menu_screen_enabled`), off by default
now (launchers, VR menu, Reset Controller). Saved settings keep their value, so
a port_settings.ini written before still detaches them until it is unticked.

- `CInGameGuiManager::Update` notes the pause menu, logbook and map
  (`PortVr::VrNoteGameMenu`, kIGGS_PauseGame / PauseLogBook / MapScreen,
  opening, open or closing) and main.cpp publishes `VRGameMode::Paused` for
  them from the note, whether or not the world was drawn. Detached, they reach
  the virtual screen on their first frame (the pause menu and logbook used to
  wait out the 45-frame FrontEnd debounce over a frozen blur quad); in view
  (`PortVr::VrGameMenuInView`), the pause blur keeps the world live as for a
  box (`CPauseScreenBlur::PortVrSetLiveWorld` now takes every paused state).
- The area texture dump is skipped in view: from gameplay, these three
  screens defer their state change until the world stops being drawn, then
  dump up to 1 MB of the room's textures (the GameCube's memory budget) and
  make the screen (`EnsureStates`). With the world live, `EnsureStates` makes
  the screen at once and keeps the textures. Brought into view after a dump
  (the setting unticked mid-menu, the headset put on), the textures are
  reloaded first and the world goes live once they all are
  (`PortVrRefreshPausedScreens`, renamed from `PortVrRefreshMessageBox`);
  meanwhile main.cpp publishes FrontEnd, so the menu waits on the virtual
  screen rather than as a frozen picture over each eye.
- The pause menu and logbook go on the boxes' screen (held, or floating with
  `vr_floating_hud`). Every eye's depth is first reset to the far plane
  (`PortVr::VrResetEyeDepth`: an orthographic quad through a (1, 1) depth
  window, compare always, depth write only).
- Their 3D models (the inventory's Samus, the logbook's artifacts, the
  options' GameCube) stand in 3D on that screen rather than flat on it:
  `PortVr::VrPauseModel3D`, constructed in `CSamusDoll`, `CArtifactDoll` and
  `CGameCubeDoll::Draw` right after their own camera is set, resets the
  eyes' depth again (over the screen's panels) and draws the model through
  `AURORA_STEREO_ROUTE_ANCHORED_3D` (or the new `FLOATING_3D` with
  `vr_floating_hud`) with a placement (new FIFO command
  `GX_AURORA_STEREO_PLACEMENT_3D`, `AuroraSetStereoPlacement3D(scale, x, y,
  z)`: the draw's camera space scaled and moved in the anchor's space) that
  puts the model's origin 0.15 m in front of the screen at the angular size
  its flat picture had (from the model camera's vertical focal term and the
  origin's depth), so it keeps its place in the menu's layout with real
  depth. Their lights are directional, so the per-eye lights need no scale.
  The Phazon Suit's shimmer on the inventory's Samus is skipped then: it is
  a pass over the whole viewport through a mask of the doll's pixels.
- The head-locked HUD (frames, reticles, minimap) is not left blurred in front
  of the eyes: it dissolves out with the game's own HUD static
  (`CSamusHud::Draw`'s alpha) as the pause blur comes up, and back in as it
  lifts (`vrHudFade` = 1 - blur amount while the world is live under the
  blur); the targeting reticles are dropped while it fades. The copy taken
  when the live view ends mid-box keeps the HUD, as the TV's does.
- The map screen is a 3D map held in front of the player. The TV map is the
  map camera's orthographic view squashed onto a plane in the HUD's space
  (depth scaled by 0.001, then 0). In view, `CAutoMapper::PortVrSetHologram`
  makes `CAutoMapper::Draw` build `planeXf` as the map camera's space scaled
  alike in every axis (its view's half-height to 0.45 m) and set 1.2 m ahead,
  drawn through the new `AURORA_STEREO_ROUTE_ANCHORED_3D` (perspective draws
  composed through the held anchor: true depth and parallax, held still; the
  anchor is held for the map even with `vr_floating_hud`). Perspective with a
  0.3 m near plane, no depth test (the map draws back to front as on the TV)
  and no minimap mask; the player icon's depth compare is off with it. The
  map screen's frame (legend, area name) and compass go on the screen behind
  and are drawn first (`CAutoMapper::PortVrDrawFrame`), so the map draws over
  them as their depths say. The map is bounded: zoomed in on a large world
  its nearest areas would reach the face or pass behind the head, so its
  depth alone is compressed until the near side of the visible areas'
  volume (`CMapWorld::PortWorldVolume`, what `RecalculateWorldSphere`
  measures; measured here too while the map opens and closes, as the game
  only measures it once open) stays 0.5 m ahead, eased out and back in
  through the universe map. The 3D map follows the map state
  (`PortVrMapInView`), not the blur's live flag: the map's own opening and
  closing outlast the 0.5 s blur, and it hands back to the HUD's minimap
  once the automapper is back in its minimap state.
  Sizes in `platform/vr/vr_view.cpp` (`kMapHologram*`).
- The 3D map grows out of the HUD's minimap and shrinks back into it, as the
  TV map does, instead of appearing at the centre: the manager takes the
  minimap's place in the HUD camera's space (with the HUD spread) at the
  head-locked route's scales and its view's half-height there, and
  `CAutoMapper::PortVrSetHologram` moves the map's centre and size between
  that and its own with the automapper's interpolation, its depth flattened
  to nothing at the minimap. The held anchor was placed where the head faced
  when the map opened, so the start is matched to where the head has the
  minimap now: `AuroraSetStereoPlacement3DFollowHead` (same command, four more
  floats) moves and turns each eye's placement that share of the way into
  the head's own space as the head-locked route has it, the point along a
  straight line and the rest about it (`stereo_replay::follow_head`, a
  quaternion share of the anchor-to-head turn), so turned away while the map
  is open, it still shrinks into the minimap where it is, facing the eyes.
  In the morph ball the minimap is on the ball HUD's screen, which the map's
  anchor holds: the map starts there instead (where the HUD camera's picture
  puts the minimap on `PortVr::VrScreenPlane`), without following the head.
- Tests: `port_vr_policy_tests` (Paused in view by default, detached at once,
  the content tag through a menu in view); `port_vr_stereo_tests` (a point of
  the head-locked space followed into the held anchor's after the head turned: all, none, half).

## VR: message boxes stay in the headset view (2026-10-11)

A modal message box (`CMessageScreen`: item pickups, "Game has been saved.
Energy fully replenished.", scan log unlocks, the 100 % items box) and the
in-game save prompt (`CSaveGameScreen`) turned the headset's view into the
virtual screen with a still picture. The user asked for the immersive view to
stay, with the head still tracked, the pause blur and the CRT scan lines over
the whole view, and the box on the 2D screen.

How it switched: not through `VRGameMode::Paused`. Nothing ever assigns
`CStateManager::kGS_Paused`, so main.cpp's Paused branch only sees the map
screen. For a box, `CPauseScreenBlur::Draw` takes one persistent copy of the
framebuffer (after the HUD and the minimap) and then clears `x50_25_gameDraw`.
`CMFGame::Draw` stops drawing the world, main.cpp publishes FrontEnd, and the
policy shows the virtual screen after its 45-frame debounce (the eyes showed the
stale blur quad meanwhile). The pause menu and the logbook still go that way, so
`vr_game_menu_screen_enabled` only ever governed the map screen.

- `VRGameMode::Message` (appended, the log prints the value as a number) and
  `PrimeVRPolicyConfig::message_box_immersive`: Immersive when on, the virtual
  screen when off, at once either way (no debounce). The flag is a safety
  change like its siblings. `PrimeVRPolicyPresentationFor(mode)` answers what
  a mode would show without publishing it.
- `CInGameGuiManager::Update` notes the box at its end
  (`PortVr::VrNoteMessageBox`: kIGGS_PauseHUDMessage or kIGGS_PauseSaveGame,
  opening, open or closing, and whether a cinematic camera is current); the
  destructor clears it. main.cpp publishes Message from the note whether or
  not the world was drawn, or Cinematic for a box under a cinematic camera, so
  that one follows `vr_cinematic_screen_enabled`.
- `PortVr::VrMessageBoxInView()`: the note, the headset running, and the policy
  answering Immersive for the box's mode. Then `CPauseScreenBlur` keeps the
  world drawn (`PortVrSetLiveWorld`, only in its message and save states, so
  the map, pause and logbook screens still dump the area textures once the
  world stops) and calls `CCameraBlurPass::FreePersistentFbTexture` before
  each draw. That only drops the "already copied" flag, so the blur copies the
  frame again with its persistent, opaque look. The copy is made under the
  WORLD route, so aurora takes it from each eye (the RGB565 conversion is the
  X-ray visor's path), and the blur quad, the tint and the scan lines are
  orthographic draws replayed identically into both eyes. Drawing the world
  while `CMFGame` is paused is safe: `PreRender` and
  `DrawWorld` only read state, and the world's clock is the simulation's.
- `CInGameGuiManager::Draw` keeps `drawVisor` on while the world is live, so
  the visor, the HUD and the minimap stay under the blur as the TV's still
  shows them (otherwise they vanished once the blur was up). The save prompt
  and the box go on the new `AURORA_STEREO_ROUTE_ANCHORED_SCREEN`: a plane of
  the menu screen's size and distance, as the morph ball HUD's, but held
  still. The first immersive frame after `aurora_set_stereo_screen_anchor`
  holds it places it upright ahead of the head's facing, at the head's
  position (`stereo_replay::upright_screen_anchor`: the head pose without its
  pitch and roll; looking straight down or up, the head's up gives the
  facing); the head then looks around it. `PortVrRefreshPausedScreens` holds it
  while the world is live, so each box and each save prompt is placed where
  the head faces when it opens; a release, or a frame that is not immersive,
  drops it, and with none placed the route draws like `SCREEN_2D`. A recentre
  or a height set while it is up places it again: the stereo packet now
  carries the tracking origin's generation (`AuroraStereoFrame::originGeneration`,
  set after `BuildPublishedFrame`'s base latch), so it is placed in the same
  centre space as the eyes that draw it. The user
  first had the morph ball HUD's easing plane (`FLOATING_HUD`) and asked for
  a fixed one. Either screen route lays the draw's mono picture on the plane,
  so the save prompt's frame (its camera 4.7 units from the text, znear 1)
  needs nothing, where a head-locked route would have put it inside the near
  plane at the default HUD distance. Their depth window is (0, 0), the
  automapper frame's: the world,
  the HUD and the minimap's depth mask are drawn in the same frame now. The
  box's panels are flat at one depth and its video band is masked to them by
  a GEQUAL test, so a single depth keeps the TV's layering (checked from the
  CMDL bounds: every panel at y 0, the band 1.68 units behind).
- The save prompt's `model_corners` sit above its picture, where the TV never
  shows them. The held plane does not crop at the picture's edges, so
  `CSaveGameScreen::Draw` hides them while the box is in view.
- Changes while a box is up: main.cpp hands the in-view answer to the blur
  before every presented frame (`CInGameGuiManager::PortVrRefreshPausedScreens`),
  since the setting or the session can change between game ticks (90 or 120 Hz
  frames, 60 Hz ticks). Leaving the live view with the box up takes one fresh
  copy after that frame's world and then stops the world, as the TV does: the
  world's own copies (refracting particles, glass, the space warp) reuse the
  spare buffer at other sizes, so the copy last taken cannot stand in. The
  visor and the HUD are drawn for that copy (`PortVrHudUnderBlur`), as the
  TV's copy holds them. With
  the setting off, main.cpp keeps the box's mode for the frame it closes on
  (it draws the world after a frame that did not), so the screen does not
  re-anchor at the head for one frame.
- Input is unchanged: `VrNoteInGameMenu` keeps the classic layout, where the
  right controller's primary button is GameCube A.
- Dismissing a box no longer recentres the view: the presentation never leaves
  Immersive, so the origin is not latched again.
- Setting `vr_message_box_immersive`, default on: F1 VR tab ("Message boxes in
  the headset view", virtual screen section), VR menu CONFIG "MESSAGE BOX IN
  VIEW" before RESET CONFIG (the calibration pages are full), both launchers'
  Calibration tab ("Keep message boxes in view") and their Reset Controller
  lists, with RESET CONFIG and the VR menu's resets.
- Tests: new `port_vr_policy_tests` (the policy had none: each mode's
  presentation, the immediate switch, the constant content tag through a box,
  the front-end debounce); `port_vr_menu_tests` (the new row, the reset row
  moved to 11, the screen anchor with a Message frame); `port_launcher_tests`
  (the key's default).
- Setting `vr_floating_hud`, default off, for the morph ball HUD and the
  boxes together: on, both use `FLOATING_HUD` (Nobbie248's easing plane, the
  ball HUD's only placement until now); off, both go on the held screen
  (`ANCHORED_SCREEN`), placed where the head faces when the ball's HUD comes
  up or a box opens; a box opened in the morph ball shares the ball HUD's
  screen, which stays with the ball's camera. Not `SCREEN_2D`, the ball HUD's
  placement confirmed on 2026-10-07: since the ball camera's entry yaw
  (10-09) and room scale (10-11) the centre space is turned by the head's
  yaw at morph time and shifted by the walked offset, so a screen fixed in it
  sits off to the side or too close. With the setting on, the screen is not
  held. F1 VR tab (virtual screen section),
  VR menu CONFIG "FLOATING HUD" (the PC's CONFIG tab now has three pages,
  RESET CONFIG alone on the third), both launchers' Calibration tab and
  Reset Controller lists. Read at draw time, so it applies at once.
- Scan lines fixed in the world: while the world is live the blur's
  `kFS_ScanLinesEven` filter (lines in 480-line picture units, so glued to
  each eye's view and moving with the head) is replaced by dark bands at
  fixed elevations on a sphere around the camera, routed
  `AURORA_STEREO_ROUTE_SKY` (the head's rotation only: they stay put, at
  infinity, without disparity). 0.8 degree pitch with the TV's half dark,
  half gap, from 88 degrees below the horizon to 88 above, the same
  0xc0c0c0 multiply faded in with the blur (`PortVrDrawWorldScanLines` in
  CPauseScreenBlur.cpp). Each band is a strip of 48 flat sides, turned half
  a side so none has a corner straight ahead: a first try with 12 sides
  sagged up to a degree between corners and showed the lines as chevrons.
- No vignette in the headset: the blur's quarters filter multiplies the view
  by `TXTR_MapLightQuarter` (white at the centre, about a quarter as bright
  at the corners) tinted toward `PauseBlurFilterColor`. While the world is
  live it is replaced by a full-screen multiply by the tint alone, which is
  what the picture's centre gets; the virtual screen keeps the TV's filter.
- Known looks to judge in the headset: the copy is half size, RGB565 and
  sampled nearest, as on the TV, so the view softens for the box's duration.

## VR: PrimedGun's own shader list, and the compile toast in the headset (2026-10-11)

The PC compiles upstream's pipeline seed in the background at startup and
shows ImGui's "Compiling shaders N / M" toast. The Quest shipped no seed (its
rows are mono, indexed, MSAA 1 configs the fork never asks for), so it
compiled each room's shaders on the first visit (up to 1.5 s stalls,
skipped draws), and no ImGui window reaches the headset, so the toast was
never seen there. PC VR got no eye pipelines from upstream's seed either.

**The seed.** `assets/primedgun_pipeline_cache.db`, built by
`tools/primedgun_seed.py` from the caches already saved on the dev PC (PC VR,
PSVR2 on D3D12, MSAA 4) and the dev Quest 3 (pulled from a debuggable APK with
`run-as`): alpha.2's cache, and the cache of a 16-minute session played with
this change's first seed (1026 configs that seed lacked). 3173 GX rows, retail
only, de-indexed only, at one sample, with the alpha.2 rows' hudSample cleared
(407 Quest rows: world materials set it before ecf519cf, and the Quest cache
held them at epoch 2 because alpha.2 ran after a HEAD build had already
purged). Every MultiviewClip row has its EyeClipImmediate twin and the other
way round, and every eye row its mono twin, so one file serves every device:
1119 mono, 1019 MultiviewClip, 16 MultiviewFull, 1019 EyeClipImmediate. A
Quest 3 keeps 2154 of them, a PC VR start or a Quest 2 2138, a flat start the
1119 mono. Rows a cache only held because aurora had
merged upstream's seed into it are dropped (the PC cache held all 2209); rows a
device recorded stay even when they normalise to an upstream config. `check`
is the ctest `port_primedgun_pipeline_seed`.

**Aurora.** A second seed slot (`AuroraConfig::pipelineCacheExtraSeed*`),
merged after upstream's: embedded in the PC executable, unpacked into
`filesDir` by the Quest activity (the APK still carries no upstream seed). A
seed whose SQLite `user_version` is not 0 is PrimedGun's: it must equal
`PortGxLayoutEpoch` (else it is ignored with a warning), only current GX rows
are taken, each takes the device's MSAA sample count (a shadow pass keeps one),
and its key is hashed again (the file's hash column is a placeholder). Only
the eye rows this device draws with are kept: MultiviewClip and MultiviewFull
when multiview is supported, requested (`vr_multiview`, pushed before
`aurora_initialize`) and MSAA is 1, EyeClipImmediate otherwise; none when the
headset won't run (`MP_PIPELINE_SEED_EYES=1` takes them anyway, to check a new
seed compiles on a desktop). Upstream's seed merges as before.

**Rows of other settings stay out of the startup compile.** A cache keeps
every config a player's settings ever produced, and aurora compiled all of
them at every start: one Quest session at MSAA 4 would have added ~1100 rows
compiled at every later start at MSAA 1, never drawn, never freed. The load
now skips GX rows whose sample count isn't the device's (shadow passes keep
one) and eye rows of the eye passes this device doesn't use (the same test as
the seed's, `gx_row_drawn_here`). They stay in the file for when the setting
comes back (then compiled when drawn; their Dawn blobs are pruned meanwhile).
A flat PC start still compiles the eye rows of the player's own VR play, so
they stay warm for the next VR start. This also stops upstream's 2209 rows
(all MSAA 1) compiling for nothing on a PC at MSAA 4.

**The headset toast.** PrimedGun v1's compile progress panel
(`BuildCompileProgressPixels`: "COMPILING SHADERS", the bar, N / M, "THIS CAN
TAKE A FEW MINUTES ON THE FIRST LAUNCH") on the VR menu's panel layer while
the menu is closed, in the image's top 384 rows (`OpenXRPanelLayer::image_rows`
crops the quad). It hangs across the top of the screen the game picture is on
(55% of its width, 5% below its top edge, 5 mm in front), or, with no screen up
(immersive play), 1.35 m ahead and 0.25 m up from where the head looked when it
appeared, upright, and stays there (a recenter or a session restart re-latches
it, as it re-places the virtual screen). Same setting and rules as the PC toast
(`show_shader_compilation`; it ends when the startup queue first drains), but
it waits a second instead of half, as v1 did, so a warm start does not flash
it. The menu takes the layer over while open.

Measured on the Quest 3 with the first seed (1635 rows; the Quest kept 1128,
desk runs, front end): cold (empty pipeline and Dawn caches, as a fresh
install) ready 64.6 s after startup on the 3-worker pool, 0 failures, slowest
811 ms; warm 1.3-1.5 s. PSS at the front end 1145 MB against 1054 MB with no
seed and an empty cache (native heap 214 against 134 MB): about 80 KB a
pipeline. The final seed's 2154 Quest rows were not timed cold (the headset
would not launch an immersive app unattended any more); by the same rates,
about 2 minutes cold, 2-3 s warm and about +175 MB PSS on a fresh install. A
player whose own cache already holds those configs pays that now anyway. PC
(RTX 5070 Ti, D3D12, 8 workers): a flat start merges the 1119 mono rows and
leaves the eye rows out; with the eye rows at MSAA 4, 2138 rows at 4x (upstream's
2209 rows at 1x skipped at load), ready 10.3 s cold, 0 failures; a warm start
with the first seed was ready in 0.63 s. A flat 1x start on that 4x cache skipped
its 1117 rows at 4x.

Not checked: the toast in a headset by eye (the user's 16-minute session logged
it shown at 410 / 941 and hidden 1.4 s after startup; desk runs sat in a dark
room with tracking lost), PC VR in a headset, a Quest 2, and how many
first-visit stalls the seed removes in play (rooms the caches never saw still
compile on the spot).

Also: `quest/Build-Quest.ps1 -Configuration debug` failed at
`preReleaseBuild` (a debug assemble configures the release variant too); the
shared-key flag is now passed for both.

## VR: no multiview on an Adreno 6xx (Quest 2, Quest Pro) (2026-10-10)

A Quest 2 player (alpha.1) saw green and red blocks among the particles of a
charged shot's impact (Frigate wall): solid squares of 2x3 to 17x12 pixels in
his video, off any grid and moving with the sparks, so drawn content rather
than video damage. The same impact (PART 88576FD3, the charged Power Beam's
default collision effect) played on the Quest 3 through the console (`fx
88576FD3 4`, frozen with `fx timescale 0` at four moments) drew right on
alpha.2 and on the branch head. On the Quest 2, MULTIVIEW EYES off made it
right too: the bug is in what the Adreno 650 (driver 512.819.0, 04/2025)
makes of our multiview shaders, which the Quest 3's Adreno 740 draws right.
Particles fetch their vertices from storage buffers in the shader, while the
world's resident geometry has native vertex input, which may be why only they
broke; not confirmed.

Aurora now leaves the DawnMultiview feature off on any Adreno 6xx
(`multiview_unsafe_on_adapter` in webgpu/gpu.cpp), so the eyes take a pass
each, as with the setting off, whatever the saved `vr_multiview` says (alpha.1
players already have `vr_multiview=1` saved). The log says "Vulkan multiview
left off on Adreno (TM) 650". `MP_MULTIVIEW_ADRENO6=1` takes multiview anyway,
to try a fix on that GPU. The in-headset menu shows MULTIVIEW EYES as
UNAVAILABLE there (and ignores the click); the F1 VR tab already greys the
checkbox out when Aurora reports no multiview; the Quest launcher's note says
so.

Cost: multiview saved about 2 ms of GPU time per frame on the Quest 3; the
Quest 2's cost in a pass per eye is not measured (its log showed 90 Hz with
GPU 50-90 % busy under multiview). Untried: the clamped storage loads
(`storage_clamp=1`) under multiview on the Quest 2, which might keep multiview.

## Merged upstream port 20ef08e3 (2026-10-10)

Upstream's 19 commits since bc73f39f (2026-10-10 13:38 to 22:29): an Active
controller selector in Remap (player one from any connected pad, f8a9fa37,
20ef08e3), a reworked Remap editor, Reset configuration in F1 System
(7f16dbb2, e52fd42b), PAL map legend icon fixes, and presentation fixes for
the morph ball and the scan freeze (#70). Three files conflicted:
CMakeLists.txt (the test list), aurora.cpp (includes) and debug_ui.cpp.

- **Settings writes.** e52fd42b reports a failed write or close instead of
  logging a save. The fork keeps its temporary file and rename (an app
  killed mid-write keeps the old settings) and takes those checks: the
  temporary file is closed and checked before the rename; one that opened
  but could not be written is removed and the save stops, since the
  in-place write after it would truncate the old file on a full disk; the
  in-place fallback (a folder that refuses the temporary file or the
  rename) checks its write and close too. A failed save leaves the settings
  dirty, which is what stops the reset.
- **Reset configuration** moves port_settings.ini, controls.toml, imgui.ini,
  controller_ports.dat, keyboard_bindings.dat and the `*.controller`
  mappings into a backup folder in the user folder and quits through the
  normal exit, so the headset shuts down as usual. The `vr_*` lines go with
  port_settings.ini: VR settings and calibration return to their defaults.
  The fork keeps `vr_cannon_texture_slot` with the disc paths
  (port_config_reset.h, tested): the cannon textures it names stay in
  `user_textures/000_primedgun_cannon`, which the reset leaves alone, and
  slot 0 would say those overrides are clear. Before its backup save the
  reset polls what the launcher wrote, as DrawUI's save does. Every fork save
  (SyncGameOptions, the VR settings and the VR menu, the background and exit
  saves) goes through SaveSettings, which `sConfigReset` stops. The Quest
  has no F1, so no reset there.
- **Active controller.** The PrimedGun pad is unaffected: it is a virtual
  status merged on port 0 after the bindings. Gamepad mode's "OpenXR
  Controllers" pad behaves as before while no Active controller is saved.
  Once a desktop pad is saved as Active controller, Aurora keeps every other
  pad off port 0, so Gamepad mode gets no input while that pad is unplugged
  (pick "OpenXR Controllers" there, or clear the choice); not changed. F1's
  controller settings no longer call the headset pad the touch overlay
  while the headset runs: its dead zones and remaps apply, for the session
  only, since Aurora saves no mapping for a virtual pad (F1 says so).
- **Interpolation (#70: 49fd705d, 05435688, 57951140).** Checked, no
  change. The camera snapshot is used only in the tick that wrote it
  (`PortSnapshotFresh`); headset frames with no tick (60 Hz simulation,
  72-120 Hz headset) keep the tick's generation and still blend, and a tick
  that skips `CCameraManager::Update` (scan-complete soft pause, pause)
  falls back to the simulation camera, levelled by `LevelVrBallCamera` like
  the other paths. `sPortCutNextUpdate` is consumed before the snapshot is
  stamped, so a snap turn still cuts. The fully morphed player now blends
  about the ball's centre; the morph and unmorph transitions keep the origin
  pivot, so the VR unmorph base still cuts as before. To check in the
  headset: above 45 degrees of roll a tick (a boost) upstream's rule snaps
  the roll while the centre stays smooth, which may judder at 90-120 Hz;
  report it upstream under #70 if it shows.
- **Left to upstream:** the Start+Back chord, F1's pad navigation and gyro
  still read enumeration slot 0 rather than the active pad, so with Gamepad
  mode's pad and a desktop pad both connected they can read the wrong one.

Tested: build/vr and build/nooxr 71/71 port tests (with upstream's
port_presentation_rules_tests and port_config_reset_tests, the latter
checking the kept cannon slot), gx_fifo_tests 247/247, and the Quest APK
built. A desktop boot to the Landing Site with VR on and no OpenXR runtime
ran clean. Not tried: Reset configuration, the Active controller selector,
and the morph ball in a headset.

## PAL discs in the launchers, vsync off in the headset (2026-10-10)

The user's answers to three of the bc73f39f merge's open questions (below):
both launchers accept a PAL disc as the game does, vsync is ignored while the
headset runs, and game options stay global.

- **Discs the launchers accept.** The game takes USA 1.00 (GM8E01) or PAL
  (GM8P01, experimental), disc 0, revision 0, and USA 1.01 and 1.02 only
  under `MP_DISC_ANY_VERSION` (platform/disc.cpp `PortDisc::Identify`,
  `IsAccepted`; 538f84f4, 12dc40e8). launcher/core/disc_probe.cpp now takes
  GM8P01 revision 0 as Ok; another revision of either disc is Wrong revision,
  and GM8J01 or a disc number other than 0 is Wrong game. `DiscInfo::Pal()`
  tells a PAL disc by its game id, whatever the check says. On the Quest,
  `QuestStorage.checkHeader` takes the same two. It gates more than the
  label: the copy's last check (DiscCopy), Play, and the game activity,
  which unsets `MP_DISC` on a problem, so the game would end at boot. The
  copy's first check is the JNI probe, i.e. disc_probe.cpp. The launchers do
  not mirror `MP_DISC_ANY_VERSION`: USA 1.01 and 1.02 still read Wrong
  revision. The PC's Play is not gated on the check, so the game still runs
  them under the flag; the Quest cannot set it. Both launchers show
  "Selected: <file> (PAL, experimental)" for a PAL disc, as the game's log
  calls its support experimental, and their select notes, setup notes and
  About tabs name USA 1.00 or PAL.
- **Card region.** The game keeps a PAL disc's saves on the EUR card,
  `<card base>/EUR/Card A`, and reads only GM8P saves there
  (`PortGci::CardRegion`, `IsGameFile`; 6295169e). The PC launcher's
  `LauncherPaths::cardFolder` became `cardBase` (`PortPaths::CardFolder()`),
  and Game Options > Open Memory Card Folder opens the selected disc's region
  (`CardFolderFor`). The Quest's `QuestStorage.cardFolder` follows the copied
  disc (`cardRegion` reads disc.iso's header). Either is USA when no disc is
  selected or its header is not read (a WBFS without a header copy).
  docs/NATIVE_PORT.md's memory card transfer text, upstream's and still
  USA-only there, now names the EUR card and GM8P files for PAL; on a merge,
  take upstream's wording once it does the same.
- **Transfer of PAL cards.** The PC's Transfer probes the selected disc and,
  for a PAL one, calls `PortGci::SetGameCode("GM8P")` first: PrimedGun.exe
  compiles port_gci.cpp but never set the code, which stayed GM8E. So
  `FindDolphinCard` looks in Dolphin's `GC/EUR/Card A` and
  `GC/MemoryCardA.EUR.raw`, and `ImportFolder`/`ImportFile` copy only GM8P
  saves, into `EUR/Card A`. The search for an old PrimedGun card
  (primedgun_import.cpp) takes the region: it scores `MemoryCardA.EUR.raw`,
  looks for `01-GM8P-*.gci` in `GC/EUR/Card A`, and skips raw cards named for
  another region (before, a USA install could pick `MemoryCardA.EUR.raw`,
  which imports nothing). The "not found" messages name the disc's region.
  After a Transfer the old launcher's selected disc is adopted only when its
  region matches the card just filled. The Quest's zip Transfer takes
  `GC/MemoryCardA.<region>(.N).raw` or the GM8P/GM8E GCIs of
  `GC/<region>/Card A` for the copied disc's region, and stages the raw as
  `MemoryCardA.<region>.raw`; a picked .raw, .gcp or .gci is staged as it
  is. The game imports pending_import into the mounted disc's region at the
  next start and skips the other region's saves. With no disc selected both
  launchers search and fill the USA card, so both setup notes say to select
  the game first.
- **The PC launcher's stored disc.** Its constructor now ends with
  `UpdateSelectedGame()` instead of `SetRunning(false)`, so a disc kept from
  the last session shows its name, PAL tag and Wrong game or Wrong revision
  warning at launch. Before, only a new pick did (since dad03036).
- **VR code with a PAL disc: checked, no change.** Nothing the fork adds to
  src/, include/ or platform/ (vr/ included) is tied to USA data: no asset,
  string-table, frame widget or DOL ids (`git diff bc73f39f de5a14bd`). The
  scan shield is loaded by name (`CMDL_ScanShield`); the sky, the title-card
  billboard and the beam wheel's items come from the level data or
  upstream's own fields; the HUD planes' tangents come from the frame
  camera at draw time, and PAL's frames differ only in text panes
  (e66acd52). The port reports NTSC and runs PAL at 60 Hz, so the headset
  pacing is unchanged. Room and world ids are the same on PAL (the cutscene
  skip tables, `MP_BOOT_WORLD 39F2DE28:B2701146`). Not verified without a
  PAL disc: whether PAL's arm-cannon textures hash as USA's (otherwise the
  cannon slots silently show the retail cannon), and the scan shield
  extension and `kHudBodyDepth`, tuned on USA's CMDL_ScanShield and
  FRME_BaseHud. Upstream gap, left alone: save states record no disc region,
  so the VR menu's LOAD NEWEST could load a state saved on the other
  region's disc. PAL has still never been played in the headset.
- **Vsync in VR** is ignored while the headset runs. debug_ui.cpp's
  `EffectiveVsync()` is the setting, but never under MP_TURBO nor while
  `PortVr::OpenXRIsRunning()`. F1's checkbox, the launcher's live edit and
  the first drawn frame all hand it to Aurora through `ApplyVsync()`, and
  DrawUI hands it again when it changes: in practice only when a PC runtime
  ends the session and leaves the game on the desktop, which the log reports
  ("vsync on now that the headset stopped"). Each call reconfigures the
  surface after a GPU sync, so Aurora is called on a change only. The stored
  `vsync=` stays, as F1 and the launcher show it, and applies again when the
  headset stops. The window only mirrors the headset, and its FIFO present
  at the monitor's rate in the render worker's end of frame would hold the
  headset loop to it (never measured). F1's Vsync tooltip, its Frame rate
  note, the PC launcher's VSync tooltip and the startup settings log ("on
  (off while the headset runs)") say so. F1's checkbox now also leaves vsync
  off under MP_TURBO, as the other two paths did. "Runs" is the pacing
  thread being alive, as for the frame caps and `HudWide()`: with the
  headset off the head the session idles, vsync stays off and the mirror may
  tear. Nothing changes in build/nooxr (`OpenXRIsRunning()` is false there)
  or on the Quest, which never presents the window while the headset owns
  the display and whose launcher has no VSync control.
- **Game options stay global** (user decision; upstream dba5bd7a). They live
  in the settings file's `game_options=` line, and loading a save keeps them,
  so the options a transferred Dolphin save carries (brightness, HUD
  opacity, invert Y, rumble...) are replaced by the global ones on first
  load, and the first start after updating begins from the defaults. Set
  them once in the game's Options. No code change, and no setup note.

Tested: build/vr and build/nooxr 68/68 port tests (port_launcher_tests
now checks GM8P01 revision 0 as Ok, revision 1 as Wrong revision, and the
region-aware card search), gx_fifo_tests 247/247, PrimedGun.exe relinked,
and the Quest APK built (Kotlin compiled). Desktop boots to the Landing
Site with the USA disc run clean (VR off, and VR on with no runtime). Not
tried: a PAL disc (none here), a Dolphin Transfer of a PAL card, and vsync=1
with the headset running.

## Merged upstream port bc73f39f (2026-10-10)

Upstream's 184 commits since 2d8d582d (2026-10-08 21:03 to 2026-10-10
13:31): experimental PAL disc support (PAL saves go to the EUR card), user
remapping (a binding engine with chords, layers and contexts, the F1 Remap
page and `controls.toml`; the beam shift is now default chords; the mouse
wheel cycles beams; a choice of button glyphs), more Remastered content
(X-ray and thermal visor posts, scan icons, Phazon Beam materials, muzzle,
charge, bomb, flamethrower and electric effects, morph ball glows), the sharp
scan window, crosshair opacity and colour, GX and FIFO speed-ups (uniforms
filled in place, a shader-info memo, bind groups keyed by handles, fewer
pipeline rebuilds, a pool of pipeline compile threads), an FPS cap slider and
a tick catch-up budget, 60 Hz movement values at any sim rate, game options
kept across launches, every setting copied into the log, a software
rendering fallback, a macOS build, the build revision as a generated source,
and upstream's own fixes for the two big-endian readers the fork had fixed.
Backup of the branch before the merge:
`backup/OpenXR-before-upstream-merge-2026-10-10`. Nineteen files conflicted;
upstream's code was also checked against the fork where git merged silently.

- **Build info and the launchers.** Upstream now generates the build
  revision as a source (074e531a, 6350679b): `generated/port_build_info.cpp`
  defines extern `kMpBuildRevision` and `kMpBuildVersion`, and the static
  `platform/include/port_build_info.h` names them `MP_BUILD_REVISION` and
  `MP_BUILD_VERSION`. cmake/BuildRevision.cmake writes the fork's
  `kMpPrimedGunVersion` into the same source (still read from
  launcher/CMakeLists.txt's `set(PRIMEDGUN_LAUNCHER_VERSION "...")`), and the
  header names it `MP_PRIMEDGUN_VERSION`. The `""` fallbacks in
  platform/main.cpp and debug_ui.cpp are gone, so a missing declaration fails
  the build, and the port_version test now wants "PrimedGun <version>
  (Metroid Prime native port". PrimedGun.exe and libprimedgun_launcher.so
  link no game code, so each compiles the generated source itself and takes
  the header from platform/include only: an older build folder still holds
  the old scheme's `generated/port_build_info.h`, whose literal macros would
  freeze the revision. The About tab passes the revision through
  `QString::fromLatin1`, since it is no longer a literal.
- **Big-endian model readers.** Upstream fixed both readers the fork fixed
  in 8796a7ab: b4dfc433 (`CSkinnedModelWithAvgNormals`, `NativeVectors`) and
  e3ad88a4 (`CAnimData`'s fallback box, `PortAccumulateModelBounds`). Both
  files are now upstream's, with one swap each, reads bounded by the arrays'
  sizes, and no `PortReadBigVector`. Take upstream's versions on later merges
  and never add a second swap (the 2026-10-07 ice beam entry says so too).
- **Input rework.** Every desktop device now goes through upstream's binding
  engine (`PortInputDevices::Poll`, then `PADSetPortOverride` on port 0). The
  PrimedGun pad is still a virtual status (`PADSetVirtualStatus`) that
  `PADRead` merges after the override, so Remap, `controls.toml`, the beam
  chords and turbo never touch it: it is not remapped. Gamepad mode's
  "OpenXR Controllers" pad is an ordinary SDL controller, read raw by the
  engine, and follows the Controllers profile like any pad. The headset-focus
  OR (2026-10-08) now sits before `Poll`, so it feeds the engine's focus gate
  as well as the clearing after `PADRead`. The motor callback is untouched.
  1f31e327 removed the old twin-stick "L is a beam shift", which turned the
  VR lock plus the visor gesture into a beam pick.
- **Direct aim stands down for the PrimedGun pad.** Mouse aim and twin stick
  read as off while `PortVr::VrPadOwnsPort0()` (vr_pad.cpp's motor claim,
  held while the controllers own port 0); 8c902112 (mouse aim) and the
  presets now turn twin stick on far more often. Twin stick would eat the
  pad's C-stick (the beam wheel's pulse, the morph ball's right stick), and
  direct aim turns the look stick's turn into a strafe and switches off the
  snap turn and directional movement. Swap Scan/X-Ray reads as off too, so the
  visor gesture keeps left for Scan and right for X-Ray. The stored settings
  and the F1 checkboxes are untouched, and Gamepad mode keeps them.
- **Glyphs.** While the PrimedGun pad owns port 0 the prompts are the game's
  own GameCube ones (`PortPrompts::ActiveDevice`). The pad sends no SDL
  events, so before, the last key or pad touched picked the set. F1's
  "Button glyphs" (`prompt_glyphs=`, 6a175aa7) still wins.
- **Spring Ball on by default.** 24419e15 moved the controls settings into
  `PortInput::Settings` with `springBall = false`. The fork's default
  (5e3ebaa8) now lives there (port_input_settings.h), and
  port_input_settings_tests checks it; fresh installs would otherwise lose
  the right stick's spring in the morph ball. The other controls keys kept
  their names, and the launchers write none of them.
- **Per-eye uniforms.** c6f0f456 fills draw uniforms in place in the frame's
  write-mapped buffer. GX draws keep the fork's three-argument
  `build_uniform`: it fills the CPU snapshot (or a scratch buffer), copies it
  once with `push_uniform` and stages the eye copies from those CPU bytes,
  because the eye copies and the draw-merge snapshot must not read back
  write-combined memory. That is still one copy fewer than before the merge.
  Upstream's in-place fill lives only in the one-argument overload, which
  nothing calls. Its `map_uniform` now keeps `push_uniform`'s headroom (a
  multiview draw binds an eye-pair window past the uniform), and a comment
  above the stereo overload gives the rules if it ever fills in place.
- **PBR uniform size.** c6f0f456 also counts `pbr_shield` in the PBR uniform
  size; 128 bytes were written past the count. That was a latent fork bug:
  `staged_multiview_mode` found the eye data off by those bytes ("Multiview
  uniform data lies ... bytes on"), so the Quest dropped every PBR draw from
  its eye passes and the PC (256-byte alignment) drew about half of them with
  the mono camera in both eyes. Remastered content had not been played in a
  headset.
- **Memo and bind groups vs stereo.** c3ea1cba's memo (shader info and
  shadow pipeline per config hash) holds mono configs only: the eye variants
  are built on a copy, and `cache.multiviewPipelineMask` is cleared before
  the lookup, so a hit never reuses the last config's eye pipelines. The eye
  variants are not memoized (below). `reset_draw_cache` clears the memo, and
  `shutdown_pipeline_cache` resets the last-pipeline shortcut that
  `touch_pipeline` trusts, for a future gfx re-initialization. edf63fc3's
  handle key moved into the fork's `build_texture_bind_group`, so the mono,
  multiview and per-eye groups all use it. The key starts with the layout,
  and eye stand-ins and array views are handles of their own, so no two kinds
  of group share an id. 19e221e2's field-by-field pipeline config keeps the
  fork's ShaderConfig bytes 10-11.
- **Eye-pass scissors** round every edge (`std::lround`), as afbc94b4 does
  for the mono mapping, so slices that share an edge in the EFB share it in
  the eye target instead of overlapping by a pixel and blending it twice.
- **`PortGxLayoutEpoch` = 2.** No ShaderConfig byte moved, and
  GXPipelineConfigVersion stays 15. But ecf519cf moved the port's HUD
  material flags out of the texture slot mask: before it, every world
  material with texture slot 0 or 1 set hudSample around its draw. Most GX
  rows that alpha.1 and alpha.2 cached are therefore configs no draw asks for
  any more, still compiled in the background at every start, now on several
  threads. The bump drops every GX row once. The cost is one cold start: the
  Quest, which ships no seed, compiles on its first room visits; the PC
  merges the re-recorded seed (853f55ee) back.
- **Remastered X-ray and thermal posts** (ca239f84, 530a5a48, 1e28066c,
  f4303c84) are encoder tasks on the mono EFB, which the eye passes don't
  replay. port_room_env.cpp's `XRayPostEnabled` and `ThermalPostEnabled` are
  false while `PortVr::VrImmersive()`: one gate for X-ray pass A
  (`CStateManager::DrawWorld`), pass B (`CPlayerVisor::DrawXRayEffect`), both
  thermal passes and the X-ray baked-light override (0.6). An immersive frame
  keeps retail's X-ray blur and thermal blends, which the fork already draws
  per eye, and retail's baked light; the virtual screen and the desktop show
  Remastered's posts. The game's call sites are upstream's again. As bloom
  does, the FIFO also skips a post recorded while `stereo_shadow::active()`,
  for a frame that turned immersive after the game asked. `XRayTick` and
  `ThermalTick` stay ungated: `CPlayerVisor::Update` doesn't run while
  paused, so a reset there would leave a paused virtual-screen frame
  mid-ramp.
- **Sharp scan window** (764058d8, 5c935ca2) is off while the headset runs:
  `PortDebug::SharpScanWindow()` checks `OpenXRIsRunning()`, as `HudWide()`
  does, and `CPlayerVisor::PortScanZoom` also refuses in an immersive frame.
  Its zoom lives in the projection's x/y terms, which the eye passes replace;
  its copy is the screen's centre, not the head-locked plane's rectangle; and
  it would draw the world once more per eye. The headset keeps the fork's
  per-eye scan window copy (magnified only with `vr_scan_zoom`), whose
  no-zoom size is now an early return in upstream's `ScanWindowCopySize`.
  `PortDrawScanOverlay` sets the WORLD route before the reticle. Chosen from
  the code, not tried in a headset.
- **Remastered scan icons with look to scan.** Upstream's icons (cabf1d5f,
  2f26d238, 927c896e) fade in only while the object is in the scan box and
  in sight. With look to scan on, the box is the head's single pick, so the
  icons use the gaze's wide cone instead (`VrLookToScanNearby`, the pick
  included), the objects look to scan already shows icons for; the scan
  frame still lights for the pick alone. While immersive the line-of-sight
  ray starts at the eyes (`VrHeadGaze`). The icons draw without depth, so the
  fork's extended scan shield doesn't mask them: they show across the view,
  as they show across the screen on the desktop.
- **Crosshair.** 00e775bf and e2fd8c65 make the opacity setting the
  crosshair's alpha (default 100%; the game's is 19%, drawn additively). In
  an immersive frame the crosshair sits in the world (on the cannon ray's hit
  with cannon targeting), and at full strength it would be about five times
  brighter than alpha.2's, so there the setting scales the game's alpha
  instead, as under Original experience. The idle crosshair keeps the fork's
  cannon-ray placement first; otherwise upstream's simulation-to-presented
  camera mapping (689c6cb1) replaces the fork's look-rotation correction.
- **Flamethrower end caps** (1254a316) fade unless the stream's tail
  projects near the screen's centre. With the tracked cannon the stream
  follows the controller, so the tail is measured in the stream's own frame
  (its emitter's orientation and position) through the camera's lens. Off
  the headset that frame is the camera and the result is upstream's. The
  caps are VFX custom draws, so for now this only shows on the mono image.
- **Frame pacing.** While the headset runs it paces the loop
  (`OpenXRWaitForFrameRequest`), and neither the 60 FPS cap nor upstream's
  FPS cap slider (4816f64b) applies; F1's Frame rate section says so then.
  Upstream's tick catch-up budget (2f72f9e3) applies in VR too: past
  max(2 periods, 1/30 s) of tick work in one frame, the remaining ticks are
  dropped and the game runs slow instead of spiralling. Interpolation stays
  on. f0f13299 (Immediate presentation on D3D with vsync off) changes only
  the desktop window's surface.
- **Game options** are global now (dba5bd7a), kept in the settings file's
  `game_options=` line rather than the save. `SyncGameOptions` saves them
  once they have rested 0.5 s: the pause menu's sliders set an option on
  every step, and the Quest's shared storage is slow to rewrite. It polls
  the launcher's edits first, as DrawUI's save does.
- **Sound mode.** sdk_stubs.cpp keeps the fork's `s_soundMode` (b476488f:
  stereo by default, follows `OSSetSoundMode`), which already does what
  upstream's 22ff478b (a constant stereo) wanted.
- **Memory card import.** A card the launchers left goes into the disc's
  region folder (`PortGci::CardRegion()`, EUR/Card A for a PAL disc,
  6295169e) instead of a fixed USA one. The launchers still took only USA
  1.00 then; they take PAL too since the entry above.
- **Checked, no change:** upstream's 60 Hz movement laws (e4a0af0b,
  c3153ed0, dfc6efb5, 4a6b3f04, e24874a2) leave the fork's directional
  movement alone, which sets the velocity itself with a ramp linear in time.
  The Remastered muzzle flashes, holo wipe and lock-on extend follow the
  tracked cannon (they draw in the gun view after its offset). The
  Remastered charge rumble goes to the headset through the motor callback
  (not felt yet). Every multiview rewrite anchor still matches after
  8351cb4b (`pbr_scr` declared once; it copies the per-eye clip) and
  5d289bcd (kind 35).

Known gaps in the headset (Remastered content; retail-only installs are not
affected): the mono-only Remastered particles of the 2026-10-08 entry now
also include 20c9974b (muzzle and charge flashes, the Wave Buster impact),
b2f7f58d (bombs), 64b66479 (ball glows and swooshes), 1254a316 (flamethrower
caps and lights, the ice spreader), f4f5554a (ELSC electric effects) and
df0d3652 (per-user replacements of shared retail effects). These are VFX
custom draws, which the eye passes skip, so they are invisible in the
headset. The Remastered X-ray and thermal posts reach only mono images (the
desktop, the virtual screen). Stereo frames still copy and hash the 2.7 KB
config for the eye pipeline on every pipeline switch, the part of
c3ea1cba's saving the Quest's FIFO thread misses. The Remastered icons'
line-of-sight test casts a world ray every tick for every remembered scan
entry, in every visor. PBR rows in view space (world up, the probe, the
ambient volume, the lightmap axes, the HDR light positions) are not
recomposed per eye; only draws with full per-eye uniform copies (a per-eye
screen texture matrix) would light from the mono camera's space, and none
is PBR today. Without look to scan, Remastered icons still pop in by the
body camera's screen box.

Not done, user decisions:

- **PAL in the launchers.** Decided: both launchers accept PAL, with
  region-aware card folders and Transfer (the entry above). PAL has never
  been played in the headset.
- **Vsync in VR.** Decided: ignored while the headset runs; the stored
  setting stays and applies again when it stops (the entry above).
- **Game options from transferred Dolphin saves.** Decided: options stay
  global, so a transferred save's options are replaced on first load; no
  setup note (the entry above).
- **Remastered VFX in the headset.** Draw imported PARTs through the retail
  GX path while immersive (VMAT-only looks and HDR colours are lost, and
  VMSH mesh effects with no PMDL draw nothing), or replay VFX custom draws
  per eye in aurora (larger work).
- **Pipeline compile pool on the Quest.** 4079fedc starts
  hardware_concurrency/2 workers (about 3 on a Quest 3), all behind Dawn's
  implicit device lock with the render worker and the XR pacing thread. A/B
  `MP_PIPELINE_THREADS=1` against the default through `debug.mport.env` on a
  cold room entry before capping it.
- **Multiview memo refs.** Keep the eye variants' refs in the memo, checked
  with `touch_pipeline`; measure with `MP_FRAME_STATS` on the Quest first.
- **Remastered icon line-of-sight cost.** Cast only for entries that can be
  drawn this frame (timer above 0, scan visor up).
- **PBR view-space rows.** Recompose them per eye if a PBR draw ever gets a
  per-eye screen texture matrix.
- **Crosshair settings in the launchers.** `crosshair_opacity` and
  `crosshair_color` are only in F1, which the Quest can't open.

Tested: build/vr and build/nooxr 68/68 port tests (port_version now wants
the PrimedGun version), gx_fifo_tests 247/247, the Qt launcher relinked,
and the Quest APK built (both native libraries carry the current revision;
no seed packaged). PC desktop boots to the Landing Site run clean in the VR
build with the headset off, with VR on and no OpenXR runtime (the desktop
fallback), and in the OpenXR-off build. The first start dropped 3975 GX
rows (epoch 1 to 2) and merged the seed's 2210 rows back, all ready 1.5 s
after startup. Not yet played in a headset (PC VR or Quest), and no
Remastered content was tried.

## VR: the widescreen HUD is off while the headset runs (2026-10-09)

A Quest 2 player on alpha.1 saw the radar and the visor selector off the HUD
frame, left of its left bracket, with the frame's top line stretched out. Not
a Quest 2 problem: alpha.1 defaults to `aspect=window` (the Quest's 1280x720
framebuffer is 16:9) and `hud_wide=1`, and neither test headset ran that pair
(the Quest 3 had `aspect=4:3`, the PC `hud_wide=0`). With those two lines added
to the Quest 3's ini, alpha.2 shows the same picture: the side clusters pushed
outward, the frame in misaligned strips, "ENERGY" cut to "ENERG".

The spread is a flat-screen layout. `CGuiFrame::Draw` warps the visor frame in
slices, each under a scissor in mono screen columns, while the widgets move by
a transform. In the headset the HUD is head-locked and re-projected through
each eye's frustum, so those columns no longer match where the frame lands in
the eye: the unspread middle slice keeps the brackets in place while the
radar and selectors slide off them. The head-locked HUD has no wide corners to
reach anyway, so `PortDebug::HudWide()` is false while `OpenXRIsRunning()`
(as `OriginalOverridesComfort` already does). The saved `hud_wide` is left
alone for the desktop, so Nobbie's default change (2e6e20a8) was not enough on
its own: alpha.1 players already have `hud_wide=1` in their ini. The pause
Options entry reads Off in the headset, as it does under Original experience.

Still open: below 4:3 (a portrait desktop window) the vertical spread takes the
same scissor path; it is not gated, since no headset build renders below 4:3.

## VR: directional movement, PrimedGun's left stick strafe (2026-10-08)

The directional movement settings (LEFT STICK STRAFE, MOVEMENT DIRECTION, the
deadzone, the speed and the two accelerations) were saved but not read: the
move stick walked forward and back and its sideways push turned Samus, as on
the TV. Now, with LEFT STICK STRAFE on (the default), the move stick works as
PrimedGun's `UpdateDirectionalMovement` made it:

- Outside an orbit lock, pushing it up walks toward where its controller
  points, and sideways strafes across that. MOVEMENT DIRECTION HEADSET uses
  the head's heading instead. Only the look stick turns her.
- Past the deadzone (0.25), her horizontal velocity is set along the stick at
  a speed ramping toward 14 units/s times the stick's push. The ramp starts
  from the speed she carries, no faster than that target, and climbs at 45
  units/s/s on the ground or 8 in the air. Easing the stick back drops the
  speed to the new target at once. The direction follows the stick at once,
  in the air too. Inside the deadzone the game's friction stops her.
- In an orbit lock the stick is the game's orbit strafe again. In a grapple
  its X turns the swing again, as with the setting off. With the snap turn on
  nothing else could turn it: the look stick only snaps, and a snap waits for
  the grapple to end. PrimedGun's lock layout, which L held on the grapple
  point gave it, did the same. With the off hand at the head (the visor
  gesture) nothing walks, whichever stick moves.
  The jump stays the game's, and the stick's up still reaches the pad, since
  the jump's arc reads it.

PrimedGun wrote the player's velocity in memory at the end of each frame, over
the game's own walk along the body. Here `CPlayer::ComputeMovement` leaves out
the stick's walk force and sets the velocity itself during the tick, so Samus
moves at exactly the ramp's speed, with no walk along the body under it.
`vr_pad.cpp` reads the stick along the heading once per frame and takes the
move stick's X off the pad's turn. `VrDirectionalMove` runs the ramp in the
tick, from the speed she carries out of the last one, and starts it again
after any tick it did not run (`CStateManager::GetUpdateFrameIndex`).

The heading is where the controller's aim pose points, levelled, in the
tracking space. That space's forward is the body's forward (the eyes and the
cannon are placed from it with no yaw of their own). It is exact however the
controller is pitched or rolled. Past about 72 degrees up or down the aim has
no heading left, so the controller's top takes over, all of it when vertical.
PrimedGun took a yaw angle out of the quaternion instead, which drifts once
the controller is pitched: aimed 45 degrees down with a 30-degree heading, it
walked 22 degrees off forward.

The gate is the snap turn's, now `CPlayer::PortVrPlayerSteers`: first person,
unmorphed, no lock, nothing disabling the input or freezing the controls, no
cinematic, no mouse aim. It also leaves out the debug free camera now, for
the snap turn too. Header-only maths in `platform/include/vr/vr_directional_move.h`
(test `port_vr_directional_move_tests`); F1 VR tab "Movement" section with a
live readout. Both launchers' movement keys lost their "not active yet" tag.

build/vr and build/nooxr: 64/64 port tests. Desktop Landing Site boot with
VR off is clean. Tried in the headset: walking, strafing and turning a grapple
swing with the snap turn on.

## VR: the controllers keep working when the desktop window loses focus (2026-10-08)

On PC, the headset controllers' buttons and sticks did nothing once the game's
desktop window lost focus (another window clicked, alt-tab, a SteamVR or
Virtual Desktop window brought up). Two gates dropped them:

- `CDolphinController::ReadDevices` zeroes every pad port while
  `SDL_GetKeyboardFocus()` is null (`docs/NATIVE_PORT.md`). That cleared the
  PrimedGun pad, which reaches port 0 through `PADSetVirtualStatus`.
- SDL ignores joystick presses while no window of ours has keyboard focus,
  virtual joysticks included, because the port sets
  `allowJoystickBackgroundEvents = false`. That froze Gamepad mode's
  "OpenXR Controllers" joystick.

The player in the headset cannot see the desktop window to click back into
it, so a running session with input focus from the runtime now counts as
focus. `OpenXRApplyControllerState` (game thread, every frame) records it for
`OpenXRHeadsetHasInputFocus` and turns SDL's background joystick events on
while it holds (`aurora_set_background_input`), off again when the session
ends or loses focus. `ReadDevices` ORs it into `inputFocused`, as it already
does for the console. While the headset has focus a desktop pad also reaches
the game with the window unfocused. The keyboard does not, because SDL resets
it on focus loss. With VR off nothing changes.

build/vr and build/nooxr: 63/63 port tests.

## VR: the lock turns the camera again unless NO CAMERA TURN ON LOCK (2026-10-08)

Look to lock-on and look to grapple kept the body's facing during a lock the
head picked, as a scan lock does. In the headset that made boss fights
impossible: the boss leaves the view as soon as strafing circles it, and the
lock cannot turn the body while L is held.

Picking and facing are now separate settings. `vr_look_lock_no_camera_turn`
(off by default) holds the facing during such a lock, as before. Off, the
head still picks the target, and the lock then turns Samus and the camera to
it as on the TV (`UpdateOrbitOrientation`, `CFirstPersonCamera`, and
`ValidateCurrentOrbitTargetId`'s look-angle test all run as in the game).
`VrLookHoldsFacing` reads the setting only for these locks; look to scan
still always keeps the facing. The setting is in the F1 VR tab's "Look
targeting" section and on the VR menu's CONFIG tab as NO CAMERA TURN ON LOCK,
after LOOK TO GRAPPLE. That row gives the Quest's CONFIG tab a third page,
holding RESET CONFIG.

`port_vr_menu_tests` checks the row, its default, the switch and RESET CONFIG.
build/vr and build/nooxr: 63/63 port tests.

## Merged upstream port 710e4f8c (2026-10-08)

Upstream's 170 commits since b02f932b (2026-10-07 20:36 to 2026-10-08 13:13):
mostly Remastered material work (PBR kinds 21-34, tangent frames, detail and
macro normal maps, wind sway, room point and spot lights, exact sRGB output,
bloom after the grade), Remastered HUD pictures, the built-in randomizer,
turbo fire, the Original experience toggle, a release update check, custom
Vulkan drivers on Android phones (adrenotools), the Adreno shader fix, a
whole-game initial pipeline cache with a compilation toast, and leak fixes
(front-end movie textures, 068db40e; Remastered room data at world teardown,
8bcdc039). Backup of the branch before the merge:
`backup/OpenXR-before-upstream-merge-2026-10-08`. Eleven files conflicted;
upstream's code was also checked against the fork where git merged silently.

- **FIFO command IDs (again).** Upstream's GX_AURORA_SET_HUD_SAMPLE
  (4d078a28), PORT_ROOM_LIGHTS (1d2f3b24, 2bb921da) and PORT_PARTICLE_FOG
  (95a91688) took 0x68-0x6A, ours since the last merge, and room lights is
  sent on every world draw, so the FIFO would have desynced on the first
  in-game frame. Ours are now 0xF000-0xF005, in a block of their own
  (`PRIMEDGUN_AURORA_SUBCMD_FIRST..LAST` = 0xF000-0xF0FF in GXAurora.h) that
  upstream's numbering does not reach. A duplicate now fails the build: a
  static_assert over every subcommand handle_aurora dispatches and another
  that keeps ours in the block (command_processor.cpp), plus a
  configure-time scan of every `#define` in GXAurora.h
  (extern/aurora/CMakeLists.txt).
- **ShaderConfig.** Upstream's fields keep upstream's offsets (bytes 0-9,
  pinned by offsetof static_asserts in gx.hpp), so the rows of upstream's
  bundled pipeline seed decode to the same configs here. Ours moved to bytes
  10-11: currentPnMtx, then a bitfield with multiview, mapBatch, mapCull and
  nativeVertices. GXPipelineConfigVersion stays 15 (the port_pipeline_seed
  test checks it), so rows that 2.0.0-alpha.1 cached read back as other
  configs. pipeline_cache.cpp drops every GX row once, before the seed is
  merged, keyed by SQLite's `user_version` (`PortGxLayoutEpoch` = 1). Raise
  it whenever our ShaderConfig bytes move without a version bump. Running
  alpha.1 again afterwards writes old-layout rows the purge no longer sees:
  delete pipeline_cache.db then.
- **De-indexed vertices against upstream's bind pose.** Upstream reads a
  skinned model's bind pose through GX_VA_TEX7 by the vertex's position
  index (bind_pos_active, the pbrBindPos fetch). A de-indexed record holds
  the position itself, so both are off under de-indexing and the shader
  falls back to its view-space height fade.
- **Multiview rewrite.** Upstream's `textureLoad` on GX textures goes through
  a new `mvLoad` helper like `textureSample` does. The module-scope
  volumetric fog helpers (`vf_*`, emitted before vs_main) read `ubuf`, which
  the multiview and eye-clip rewrites rename, so every fogged draw's eye
  pipeline failed to compile (a gap since the volfog came in with the last
  merge): they now read the mono uniform or the first eye copy.
- **Sun shadow casters from the geometry cache.** vs_shadow now declares the
  native vertex inputs it reads, and gfx/shadow.cpp binds the geometry
  buffer's group, its vertex buffer and 32-bit indices for cached casters
  (pre-existing with Remastered sun shadows). Still open, in upstream too: a
  caster is a copy of the draw taken before strip or surface merging, so it
  casts only the first piece of a merged run.
- **Bloom and grade in the headset.** The post process's in-pass composite
  made the eye passes clear the world (eye passes run neither encoder tasks
  nor custom draws). Immersive frames now skip bloom and grade; they keep the
  exposure measurement only when the mono image is drawn anyway (not on the
  Quest, nor with a desktop mirror showing the eyes), since the break would
  bring back a whole mono render.
- **Quest build.** No custom Vulkan drivers: the OpenXR runtime creates the
  device, and linkernsbypass patches the dynamic linker from a load-time
  constructor. adrenotools is linked only into Android builds without
  OpenXR (`MP_CUSTOM_GPU_DRIVERS`), elsewhere port_gpu_driver is stubs. The
  Adreno shader fix's Auto, which upstream turned on for every Adreno 7xx,
  means Off on the Quest until an A/B on the headset shows the clamp is
  image-identical and not slower (`MP_STORAGE_CLAMP=1` through
  debug.mport.env). The APK does not ship upstream's seed: its rows are the
  phone's and desktop's mono configs, mostly indexed, which the headset never
  asks for, at ~2249 background compiles per start. (Since 2026-10-11 the
  APK ships PrimedGun's own seed, `assets/primedgun_pipeline_cache.db`, and
  no longer reads `quest/assets/`: see "PrimedGun's own shader list".) No pointer
  capture on the Quest (a98aa72c is for phones' navigation bars).
- **Update check.** Upstream's asks Odrannnn/MetroidPrimePort about
  upstream's versionName, so it would point PrimedGun players at the flat
  port: it is off (ApplyUpdateCheck), and F1's Updates section links to
  PrimedGun's releases. The check itself stays in the tree for small merges.
- **Version.** The build info also carries `MP_PRIMEDGUN_VERSION`, read from
  launcher/CMakeLists.txt's `set(PRIMEDGUN_LAUNCHER_VERSION "...")` (keep
  that form). Since the bc73f39f merge the generated port_build_info.cpp
  defines it as `kMpPrimedGunVersion`, and platform/include/port_build_info.h
  names the macro. --version, the log header and F1 About show "PrimedGun x
  (native port y), build z"; MP_BUILD_VERSION stays upstream's.
- **Original experience.** While the headset runs it leaves the helmet and
  the frame, actor, pose and particle interpolation to their own settings
  (the helmet is head-locked in VR and the headset presents above 60 Hz).
  The minimap row follows the toggle like the helmet's.

Known gaps with Remastered content in the headset (all predate this merge,
none affect retail assets): the Remastered particles, water and the
volumetric fog's full-screen apply are custom draws or encoder tasks, which
reach only the mono image; the volumetric fog's froxels follow the game
camera; refraction glass samples mipped screen copies (b5008e57) whose eye
stand-ins have one level; the probe-face capture pass is replayed per eye.
Upstream bugs seen during the merge, to report rather than diverge on:
SetStaticArraysCurrent's TEX1-TEX7 loop overwrites the skinned bind pose in
GX_VA_TEX7; GX's 2:1 copy mipmap flag now builds a mip chain for every copy;
PortRoomEnv::ProbeOverrideScope does not nest (CPlayerGun::Render and
CGrappleArm::Render); shadow casters miss merged draws (above).

Tested: build/vr and build/nooxr 63/63 port tests, gx_fifo_tests 247/247.
PC desktop boots (Landing Site, Save Station Magmoor B) run clean at 120 FPS
and log the purge (4881 rows on the dev PC). Quest 3: the first start purged
6401 rows; the Landing Site runs immersive at 90/90 FPS with no stale frames
and no shader compile failure or missed multiview anchor; Save Station Magmoor
B stays at PSS 1.19-1.28 GB with 66-86 render targets (the eye-copy fix
holds), and the native heap is ~100 MB lower without the seed's compiles.
Not yet played in a headset: PC VR (D3D12) and Remastered content.

## VR: eye copies of the fog volumes' EFB chunks filled the Quest's memory (2026-10-08)

On the Quest, after about 20 minutes of play ending in Magmoor Caverns, the
sound crackled, the image froze for up to 1.4 s at a time (pipeline compiles
took 1.5 s) and the game was gone. Android's exit record (`dumpsys activity
exit-info org.primedgun.v2`) shows a SIGKILL while in the foreground with
PSS = RSS = 3.8 GB. The kernel OOM killer had not run (`oom_kill 0`), lmkd
would have reported LOW_MEMORY, and the game's log just stops: Horizon OS
reclaimed the memory after the system had thrashed.

Metroid Prime's fog volumes copy an EFB chunk the size of the volume's
projected box into two fixed buffers, so the copy size follows the view.
Upstream's f9983a8b (2026-10-03) bounds the mono copies: `copy_tex` keeps the
16 sizes a destination wrote most recently and frees the rest. In stereo
replay every one of those copies also gets two eye-sized stand-ins
(`stereo_shadow`), and those were only freed by a sweep every 600 frames
(6.7 s at 90 Hz), long after their mono copy was gone. A headset never holds
still, so a fog room makes a new size nearly every frame. Dawn also keeps
freed memory in its pools, so each fog room raised a peak that never came back
down.

Measured on the Quest 3 with the debug console (`adb shell setprop
debug.mport.env 'MP_CONSOLE=4777 MP_LOG_COPY_TEX=1'`, `adb forward tcp:4777
tcp:4777`): warping from Transport to Phendrana Drifts South into Save Station
Magmoor B, whose two fog-volume buffers get up to 85 new sizes a second even
with the headset on a desk. With the v2.0.0-alpha.1 APK, live render targets
reached 846 (1.2 GiB) within a minute, GPU memory 1.75 GB and PSS 2.54 GB
(2.1 GB once the sweep ran). With this fix, in the same minute (1292 new mono
copies against 1327, still "4 EFB copies (4 taken per eye)"): 66-86 render
targets (at most 46 MiB), GPU memory 530-617 MB and PSS 1.33-1.42 GB flat. A
360-degree turn in 1-degree steps in the same room (812 new copies) stayed at
PSS 1.29-1.48 GB with no upward trend.

- `stereo_shadow`: every lookup or write stamps the entry with the frame, and
  `begin_frame` drops entries each frame once the mono copy is gone or has
  gone 4 frames neither written nor looked up. A stand-in that is still drawn
  is looked up each frame (the per-eye bind groups are rebuilt per epoch), so
  only stale ones go; a mono copy sampled again later without a new copy
  falls back to the mono texture in both eyes, as before its first eye copy.

Not addressed here: the pipeline compiles of up to 1.6 s on first entering
new rooms (upstream's whole-game pipeline seed, e55e8b6a, is a candidate) and
upstream's front-end movie texture fix (068db40e).

## VR: look to lock-on and look to grapple (2026-10-08)

Outside the scan visor, the L lock still picked its target the TV's way.
`CPlayer::FindBestOrbitableObject` takes the objects inside the orbit zone of
the first-person camera's screen, and in the headset that camera carries only
the body's yaw. An enemy or a grapple point the head looked at could not be
locked unless the body faced it. PrimedGun picked these targets with the
cannon's aim ray (`PickGunRayTarget`). The port now picks them with the
head's gaze, as look to scan does for the scan target.

- `vr_look_to_lock_on` (enemies and every other lockable object) and
  `vr_look_to_grapple` (grapple points), both on by default, each hand their
  kind of target to the head. They are in the F1 VR tab's "Look targeting"
  section and on the VR menu's CONFIG tab after LOOK TO SCAN. The launchers
  do not show them, as they do not show look to scan.
- `platform/vr/vr_look_scan.cpp` measures the `kMT_Orbit` objects around the
  gaze once per tick (`VrLookTargetUpdate`, from `CPlayer::UpdateOrbitInput`).
  It uses look to scan's measures, target cone and score; a grapple point gets
  PrimedGun's 1.3 times wider cone (`LookScan::LockConePerp`). Reach and radius
  are look to scan's (`vr_gun_targeting_distance`, `vr_gun_targeting_radius`).
  Objects without the target distance test (bosses) are measured 1.75 times
  further, as PrimedGun's gun ray did.
- The game's rules still apply: `ValidateOrbitTargetId`, the orbit target
  distance, line of sight, and never the current target. A grapple point also
  needs the Grapple Beam, the orbit distance, and for a swing-locked point the
  player in its swing plane (`FindBestOrbitableObject`'s rules).
- `CPlayer::FindOrbitTargetId` returns the head's pick. When the head picks
  nothing, the screen box's choice stands if it is of a kind the head does not
  pick (a grapple point ahead of the body with look to grapple off, say).
- During a lock the head picked, the body keeps its facing, as during a scan
  lock (`VrLookHoldsFacing`, formerly `VrLookToScanHoldsFacing`). The camera
  and Samus do not turn to the target, a turn under way stops (nothing damps it
  during a lock), and `ValidateCurrentOrbitTargetId` skips its look-angle
  test. Strafing still circles the target, since `ComputeDash` works from the
  orbit point. The grapple's pull and swing still set Samus's facing.

The look-angle test was also latent in a scan lock. A scan lock that started
with the target dead ahead set `x374_orbitLockEstablished`, and could then
break once strafing took the body's facing more than the orbit angle off the
target.

`port_vr_look_scan_tests` checks the cone, `port_vr_menu_tests` the rows, the
switches and RESET CONFIG. build/vr and build/nooxr: 59/59 port tests.
Confirmed in the headset.

## VR: the snap turn (2026-10-08)

The SNAP TURN setting used to switch off smooth turning on the look stick and
nothing more; SNAP TURN ANGLE was saved but not read. Now, with SNAP TURN on,
a flick of the look stick turns Samus by the angle at once, as PrimedGun's
`UpdateSnapTurn` did:

- The flick counts past 0.72 on the stick's x, and the stick has to come back
  under 0.32 before the next one. Two snaps are at least eight game frames
  (0.133 s) apart.
- A flick made while the stick's controller is at the head (the visor
  gesture's zone) never counts, even once the hand leaves with the stick still
  pushed. A flick made while Samus cannot turn (menu, map, cinematic, morph
  ball, visor change) is used up rather than kept.
- The angle is clamped to PrimedGun's 30-90 degrees.

`platform/include/vr/vr_snap_turn.h` holds the tracker (test
`port_vr_snap_turn_tests`). `vr_pad.cpp` feeds it before the visor gesture
zeroes that stick. `CPlayer::PortVrSnapTurn`, from `CPlayer::Think`, applies
the turn with the touch look's guards plus no orbit lock or grapple, since
there the game sets the facing. PrimedGun rotated the player's transform in
memory; here `SetTransform` does it during the tick.

Two things would otherwise sweep where PrimedGun jumped:
- the frame blend: `CCameraManager::PortCutNextUpdate` makes the next camera
  update a cut; a turn under 45 degrees used to blend across one tick;
- the cannon's 0.24 smoothing, in world space: `VrResetCannonSmoothing` drops
  it, as PrimedGun's snap did.

One difference from PrimedGun: it read the right controller's stick whatever
the stick layout, so with the sticks swapped its snap turn sat on the movement
stick. Here it is the look stick, the one smooth turning reads; the default
layout behaves the same.

## Renderer: a lean texture bind group for plain GX draws (2026-10-07)

After the upstream merge, the Chozo plaza encoded in 2.33-2.50 ms on the
Quest's render worker against 2.0-2.2 ms before, and the FIFO's bind-group
stage took 0.68 ms against 0.62. Upstream's PBR, volumetric fog, shadow and
lightmap work had grown GX group 2 from 23 to 27 bindings, and every draw
bound all of them; Dawn tracks each texture of a bind group every time one is
set.

A GX shader without PBR or volumetric fog declares nothing past the GX
textures (bindings 0-15), so such draws now take a Lean layout of those 16
entries (`gx::TextureLayout`, `texture_layout`): its own bind group layouts
(mono and multiview), pipeline layouts and empty groups, beside the Full and
shadow-receiver ones. A draw without a group of its own inherits the bound
group only in its pipeline's layout; otherwise the encoder puts that layout's
empty group back (`gfx::bind_gx_textures`). The choice is a pure function of
the ShaderConfig, so the pipeline cache stays valid. `build_pipeline` checks
that it never gets a null layout: Dawn would infer one from the shader and
the draws would come out as garbage, not as an error.

Measured at the plaza (72 Hz, scale 0.85), the lean build against the merge
build interleaved in one sitting, memory clock mostly 2092 MHz: encode 1.74 /
1.84 ms against 2.35 / 2.36, the FIFO's bind-group stage 0.58 against 0.68,
72 / 71 FPS with 0.5-1.4 stale frames a second against 72 / 69 with 1.2-3.7.
On the PC (D3D12, windowed), encode 0.46 against 0.72 ms and submit 0.42
against 0.54. The image is unchanged on both. (A Quest shot taken with the
controllers asleep shows the arm cannon across the view: the cannon's default
pose, not a rendering fault.)

Upstream's draw tags (GX_AURORA_SET_DRAW_TAG, two FIFO commands per model
surface) have a toggle, `draw_tags` (F1 > Debug > Rendering, and DRAW TAGS on
the VR menu's DEBUG tab). Off saves only 0.05-0.1 ms of FIFO time, so they
stay on by default.

Measuring notes. The Quest's memory clock (VrApi `Mem=`) ranged from 1555 to
3196 MHz across sessions and moved every memory-bound stage (the FIFO's
copies and de-indexing, encoding) by 10-30 %: most of the "merge regression"
below was that, the rest the bigger bind groups. Compare builds interleaved in
one sitting and record `Mem=`. The MP_FRAME_STATS milestone lines now carry
the game thread's phases (update, world draw, HUD draw). Two switches of a
first A/B never reached the game: the Quest activity forwards only the `MP_*`
names in `PrimedGunVrActivity.TEST_ENVIRONMENT`. `MP_DAWN_ENABLE` and
`MP_DAWN_DISABLE` are forwarded now, so `--es MP_DAWN_DISABLE skip_validation`
runs the Quest with Dawn validation on.

## Merged upstream port b02f932b (2026-10-07)

This merge brings in upstream's 552 commits since the last merge (136ceb5c,
2026-10-03). Among them is upstream's fix for the CSfxManager stack overflow:
71 or 72 live sounds, for example a swarm dying with its pickups, aborted
the Quest build. Upstream also built several things in the same places we
had, so these are reconciled rather than doubled:

- **FIFO command IDs.** Our GX_AURORA_STEREO_DRAW_ROUTE, STEREO_HEAD_LOCKED_PLANE,
  STEREO_SCREEN_TEX_MTX, MAP_BATCH, CALL_CACHED_DL and FREE_GEOMETRY_SET
  (0x50-0x55) had the same numbers as upstream's new commands. The processor's
  if-chain sent one side's commands to the other's handler: the first Quest
  run crashed reading a cached display list's address from a draw tag. Ours
  then moved to 0x68-0x6D, after upstream's last ID (0x67), which upstream
  reached the next day: they now live in a block of their own (merge of
  2026-10-08 below).
- **Texture group cache.** Upstream's bind_texture_group and our
  bind_gx_textures both remembered group 2. Ours is kept, with upstream's
  rule: a draw without textures doesn't inherit a shadow receiver's group
  (its own layout). The empty group, or the multiview one in a multiview pass,
  goes back instead. RmlUi still calls forget_texture_group.
- **Immediate data.** Upstream's draw serial (the drawid view) and our
  multiview eye mask both took the one spare u32 of DrawImmediateData, which
  must stay 64 bytes. The serial keeps bits 0-23 and the eye mask takes bit 31
  (set_eye_mask). The multiview shader rewrite reads `imm.serial >> 31u`.
- **ShaderConfig.** Our multiview, map batch and map cull bits fill the
  first bitfield byte. Upstream's drawId and shadow bits get a byte of their
  own. Our currentPnMtx and nativeVertices follow upstream's four new bytes,
  so the header is 12 bytes and the struct still has no padding. (Moved again
  by the 2026-10-08 merge so upstream's bytes keep upstream's offsets.)
- **Multiview layouts.** The multiview texture group layout is now built from
  the same entries as the mono one, with only the GX textures as 2D arrays.
  It therefore has upstream's lightmap, BRDF table and volumetric fog
  bindings, and a shadow receiver variant. The shader rewrite turns only
  `tex0`-`tex7` into arrays: the BRDF table stays 2D. Stereo variants are
  built without vs_shadow, because the shadow map pass draws mono.
- **Overflow guards.** Both sides made a full frame buffer drop a draw
  instead of aborting, and both stay. Upstream's OverflowRange covers every
  push. Our staging_room checks cover the strip-merge path, which writes
  into the staging directly. push_uniform keeps room for two uniforms (a
  multiview eye pair).
- **Store discards.** Upstream's discard_dead_stores applies to the mono
  pass. Eye and multiview passes always store their colour, because the
  multiview attachment has no resolve target. They mirror the mono pass's
  depth store.
- **Resident display lists** (GXPortRetainResident, room geometry mods) hold
  raw indexed vertices. With CPU de-indexing on (the VR default), they are
  processed as if sent.
- **Crash handler.** Upstream's PortCrash, which also covers Android and Linux
  and the hang watchdog, replaces our Windows-only crash_handler.cpp. The one
  feature lost is MP_CRASH_FULL_DUMP.
- **Kept from our side:** spring ball and hide helmet on by default, the
  anisotropy of 4 on the Quest, and the VR page in upstream's new F1 layout.

Validation: 405 aurora tests and 58 port tests pass. The PC VR,
desktop-without-OpenXR and Quest builds compile. On the Quest 3, the Chozo
plaza draws the same image as before the merge (722 draws, 0 errors,
0 dropped draws).

At 72 Hz and scale 0.85, the merged build first measured 70 FPS with about 2
stale frames a second at the plaza, against 71-72 FPS before, with the FIFO
thread 0.2 ms and encoding 0.15 ms slower. See the lean texture bind group
entry above: most of that was the headset's memory clock, the rest the bigger
texture bind groups, and both are resolved.

An older build run after this one recompiles shaders on a background thread
for the whole session. Upstream moved dawn_cache.db to schema 3, with blob
checksums, and the old build's cache fails against it. The busy thread
holds the CPU clock at 2208 MHz, so its FIFO numbers look 1 ms better than
they are.

## Ice beam shell spiked to infinity: averaged normals read big-endian (2026-10-07)

Freezing an enemy with the ice beam sometimes drew its ice shell as long
streaks shooting out of the body to infinity, all parallel to the shot. Space
pirates showed it most. PC and Quest both had it; the console does not.

The frozen shell is the character's ice model drawn through
`CVertexMorphEffect`, which bulges the shell along the shot's direction. On
its first draw after a freeze it picks the vertices whose normal faces the
shot and gives each a weight from that normal. The normals come from
`CSkinnedModelWithAvgNormals`, which groups the ice model's vertices by
position and averages their normals. That constructor read the model's
position and normal arrays as native floats. The port keeps those arrays as
the file stores them, big-endian (`CSkinRules::PortBuildPointsAndNormals`
swaps each float as it skins, and `CCubeModel::SetSkinningArraysCurrent` says
so), so every value was a byte-swapped float: mostly denormals and zeros, a
few huge. Distinct positions then compared equal within `FLT_EPSILON` and were
grouped together, and the summed normals normalised to 0/0 or x/0. A NaN
normal drops its vertex from the effect; an infinite one gives an infinite
weight, and the vertex is pushed to infinity along the shot. That is the
streak.

Measured at the Phazon Mines save station (slot 2 save state) with a
temporary count at load: one 134-vertex ice model had 101 of its averaged
normals not of unit length and 65 position groups; another, 371 vertices, had
261 bad and 274 groups. With the fix every averaged normal is unit length and
each vertex has its own position group (134 of 134, 371 of 371).

- `CSkinnedModelWithAvgNormals`: under `TARGET_PC`, swap the positions and
  normals into native vectors once and average those. The console path is
  unchanged.
- `CAnimData`: the constructor and `SubstituteModelData` accumulate
  `x108_aabb`, the fallback bounding box of a character with no
  per-animation boxes, from the same raw position array. Same swap.
- Since upstream's NBT normals (ff4aa620, 7ecee577: N, B, T and sometimes a
  second B, T per entry), the swap reads each vertex's N at `NormalVecs()`
  vectors per entry, and `nativeNormals` keeps one vector per vertex.
- Superseded since the bc73f39f merge (2026-10-10): upstream fixed both
  readers itself, b4dfc433 (`NativeVectors`) and e3ad88a4
  (`PortAccumulateModelBounds`), bounded by the arrays' sizes. Both files
  are now upstream's and the fork's loops and `PortReadBigVector` are gone.
  Take upstream's versions on merges and never add a second swap.

Confirmed in the headset by the user: no more spiking ice. 42/42 port tests in
`build/vr` and `build/nooxr`.

## VR: the morph ball's HUD on the virtual screen (2026-10-07)

In the headset the morph ball's HUD sat at the edges of the view, half out
of sight, while the combat HUD worked. The combat HUD is drawn through a
perspective camera, so the head-locked route places it at its own angles. The
ball HUD's frame (`FRME_BallHud`) has an orthographic camera (PrimedGun's
classifier recognises it by its -3.2 left edge). An orthographic draw is
replayed unchanged in each eye, so it stretches over each eye's whole image.
Its energy bar and bomb gauges then land at the edges of the lenses, and in a
different spot in each eye because the eye frustums are asymmetric.

Aurora's reserved route `AURORA_STEREO_ROUTE_SCREEN_2D` now lays a draw's mono
picture on the virtual screen. The screen hangs `vr_screen_distance_meters`
ahead of the game camera and is `vr_screen_width_meters` wide, the same
settings as the menu and cinematic screen, with the picture's aspect. It is
Wiicompiled's "race 2D screen" (`HudScreen`): it stays in front of the
player, and the head can turn to look at its corners. Lean back and
recentring move it as they move the world.

- `stereo_replay::compose_screen_2d_projection` takes perspective draws as well
  as orthographic ones. It builds the screen point from the mono clip
  coordinates, (x * halfWidth, y * halfHeight, -distance * w, w), so the whole
  chain stays a single matrix. Depth uses the mono clip z times the eye depth
  of the screen's nearest corner. The draw's own NDC depth is therefore scaled
  by a factor of at most one. Every draw at a given point of the screen gets
  the same factor, so the layout keeps its depth order and its depth range,
  which keeps it in front of the world. A screen that faces the eye keeps the
  exact depth. A draw in a sub-viewport is remapped into the picture, and the
  remap is undone for the eye pass, which applies that viewport again.
- `aurora_set_stereo_screen_2d(width, distance, unitsPerMeter)` is read at
  frame begin. `PushVrSettingsToAurora` feeds it the screen settings and
  `vr_world_scale`. With no screen, the route draws like FULLSCREEN, as before.
- `CSamusHud::Draw` routes the ball frame and the base frame drawn with it
  (hint messages, counter) to the screen while `PortVrBallHudShown()` (the HUD
  state is `kHS_Ball`). `CInGameGuiManager::Draw` sends the minimap there too,
  so it keeps its place in the ball HUD's layout. The camera filters stay
  full-screen. The combat, scan, X-ray and thermal HUDs are unchanged.
- The stereo statistics line counts "screen 2D" draws.
- `port_vr_stereo_tests` checks the following: a screen facing the eye matches
  the head-locked plane; a perspective draw lands on the screen with its depth;
  on a turned screen, depth only shrinks, alike for every draw at a point and
  exact at the nearest corner; a sub-viewport draw keeps its place; and the
  eye uniform carries the projection. Picking the farthest corner instead of
  the nearest fails the depth check.

build/vr and build/nooxr pass 42/42 port tests, and a desktop boot to the
Landing Site runs clean. Confirmed in the headset (PSVR2, SteamVR): the ball
HUD shows on the screen, with 12 draws a frame on the route in morph ball and
none in first person.

The same headset session turned up two more things.

- **Region title cards.** On arrival in a region, a script billboard
  (`CScriptSpecialFunction` `kSF_Billboard`, which shows `TXTR_LavaBillboard`,
  `TXTR_ChozoRuinsBillboard` and the like from NoARAM.pak) sets
  `CStateManager`'s pending on-screen texture.
  `CInGameGuiManager::Draw` draws it with `CGraphics::Render2D`, which is
  orthographic, so it was stretched over each eye's image too. That draw now
  takes the screen route.
- **A message box looked like a freeze.** "Increased Pirate activity in
  Magmoor Caverns." waits for GameCube A. Because the player was still in
  first person and unmorphed, `vr_pad.cpp` kept the gameplay layout, where A
  is the weapon trigger and the controller's A does nothing. PrimedGun
  switched to its default controls through game flow hooks for the message,
  save, logbook, pause and map screens. Here
  `CInGameGuiManager::Update` tells the pad every tick whether one of its
  paused states (`kIGGS_MapScreen` to `kIGGS_PauseHUDMessage`) is the
  previous or the next state (`PortVr::VrNoteInGameMenu`), and the classic
  layout applies meanwhile. The diagnosis came from the thread stacks of the
  live process, read with dbghelp, and a capture of the mirror window. The game
  loop was running; its world pass was replaced by the pause blur and the box.

Confirmed in the headset: the Chozo Ruins title card shows on the screen (one
draw a frame on the route while it is up). The menu layout for message boxes
has not been tried on a message yet.

## Renderer: a full frame of vertices no longer aborts the game (2026-10-07)

A Quest play session crashed after a door load. The crash was a SIGABRT in
`ByteBuffer::append`, called from `push_verts` on the FIFO thread. A
virtual-screen transition had filled the frame's 5 MiB of vertex staging
(4.6 MiB used) before one large draw. That staging is mapped memory, which
cannot grow, so the append aborted.

- **Guard.** The FIFO processor now checks for room before it stages vertices
  or indices. A draw that does not fit is skipped and logged as "Frame staging
  full" (the first 20 such frames, then every 300th). New geometry-cache surfaces
  wait for a later frame when less than a quarter of the vertex staging would
  remain. `ByteBuffer::resize(0)` now keeps the capacity. Before, an empty
  append dropped it to 0, and the next append on mapped memory aborted.
- **Room.** A frame now holds at least 10 MiB of vertices
  (`MinVertexBufferSize`), so the transition fits and nothing is skipped. Mods
  with room geometry still scale from 5 MiB, so their buffers keep their
  sizes. `aurora_get_frame_buffer_scale` reports the scale it was given
  instead of inferring it from the vertex size. The cost is about 30 MiB of
  memory: 5 MiB in each of the five staging buffers and in the device buffer.

Validation: 236 FIFO tests pass, including a new one that fills a fixed
staging buffer. The 42 port tests pass. After the guard, a Quest walkthrough
through the loading zone that crashed did not crash. The log showed one draw
skipped during the transition frames; the 10 MiB floor is meant to remove
that, and the zone has not been walked through again since. Two same-sitting
5 MiB/10 MiB Quest runs at the Chozo plaza (scale 0.85, 72 Hz, 492 MHz) gave
App GPU 6.36/6.39 ms vs 6.39/6.40 ms. FIFO and encode times were equal, with
no staging warnings. App GPU varies 5.7-6.4 ms between sessions at the same
clocks, so compare builds in one sitting.

## VR: the VR menu's CONFIG and DEBUG tabs (2026-10-06)

Two tabs of the port's own join PrimedGun's six in the headset's VR menu:
CONFIG, next to LAYOUT, and DEBUG, last. Eight tabs do not fit PrimedGun's
150-pixel tabs, so each tab is as wide as its label plus 12 pixels a side. The
strip is centred in PrimedGun's.

The game's frame rate shows beside the title on every tab ("90 FPS"). For the
three seconds of SETTINGS SAVED, the notice takes its place. It
counts the game's own frames (`CGraphics::EndScene`) rather than using
`aurora_get_fps`, which counts the desktop window's presents. A Quest has no
window, so that read 0 there.

**CONFIG** holds the launcher's Port Config VR settings and the renderer
switches the F1 VR tab has. It spans two pages (three on the Quest).

| Group | Rows |
|---|---|
| Both platforms | RENDER SCALE, EYE RESOLUTION (a readout), REFRESH RATE, WORLD SCALE, DRAW THE WORLD PER EYE, REMOVE CINEMATIC BARS, SKY AT INFINITY, SPACE WARP, SCAN WINDOW ZOOM, SCREEN DISTANCE, SCREEN WIDTH, LEAN BACK, PIPELINED RENDERING, INDEXED VERTICES ON CPU, LOOK TO SCAN, LOOK TO LOCK-ON, LOOK TO GRAPPLE, NO CAMERA TURN ON LOCK, BEAM WHEEL HUD HIGHLIGHT, RESET CONFIG |
| Quest only | FOVEATION, PERFORMANCE LEVEL, PASSTHROUGH, MULTIVIEW EYES, DIRECT TO HEADSET |
| PC only | WINDOW SHOWS (the mirror) |

- EYE RESOLUTION shows the eye size the chosen scale gives (`OpenXRGetEyeResolution`).
- Choices get the value box's -/+, which stop at the ends; a click elsewhere on
  the row cycles on.
- Two rows are labelled "NEXT START": DIRECT TO HEADSET always, and FOVEATION
  when the session started without density maps.
- RESET CONFIG takes two clicks and leaves PrimedGun's settings alone.

**DEBUG** holds what of the F1 Debug tab works in the headset, plus live
readouts:
- FULL HEALTH, and GRANT EVERYTHING (two clicks). Both read "NO GAME" outside
  a game.
- INVULNERABLE, STREAMED AUDIO, MUSYX AUDIO, WRITE THE LOG TO A FILE, and XR
  DIAGNOSTICS LOG.
- Readouts: headset refresh, new frames to the headset, game frames per second,
  draws per frame. They are sampled twice a second, so the image is redrawn at
  most that often.
- The free camera and the modding tools need a pad and the desktop, and stay in
  F1.
- `PortDebug::CheatFullHealth` and `CheatGrantEverything` now hold the F1
  buttons' code, for both callers.

**Live from the menu.**
- Render scale changes reach the headset at once. `PushVrSettingsToAurora` now
  calls `OpenXRSetRenderScale`, and the backends rebuild the eyes at the new
  size.
- A changed performance level is asked for again on the pacing thread
  (`OpenXRReapplyPerformanceLevel`), not only at the next session.

**Tests.** `port_vr_menu_tests` adds the tab strip, both platforms' CONFIG rows
and pages, the steps, the choices, the readout and the reset. It also covers
DEBUG's cheats, two-press grant, switches and readouts.

## VR: PrimedGun's in-headset menu (2026-10-06)

PrimedGun's VR settings menu now opens in the headset, the way PrimedGun opened
it. The trigger is a click of the off hand's thumbstick (the left one unless
left-handed) or that hand's menu button. The other stick's click stays "SET
HEIGHT", as on the menu's own Layout page. The game keeps running, and the
controllers are withheld from it until everything is released after the menu
closes.

**The same image.** `platform/include/vr/vr_menu.h` is PrimedGun's
`PrimedGunOverlayCommon.h` raster: a 1024x512 canvas drawn on the CPU with its
5x7 bitmap font and solid rectangles that replace alpha. PrimedGun's colour
constants are kept unchanged. They read like amber hex (`0xE0FFB030`), but
PrimedGun uploaded the little-endian words into an RGBA8 swapchain, so the
headset showed `#30B0FF`. The port uploads the same bytes, so the menu is the
blue one players saw. The `?` of "ARE YOU SURE?" is still missing (the font has
no glyph for it) and the culling cone still reads "115.00".

**Placement and pointer.** The menu is a quad layer on the off hand's grip
(1.05 x 0.72 m, PrimedGun's offset), or 4 x 2 m latched 2.7 m ahead when
"DETACH VR MENU FROM HAND" is on. The cannon hand's laser (8 mm, warm yellow,
up to 8 m) and its 2 cm hit dot are two more quads. They are cut from a sprite
strip below the canvas in the same swapchain image (1024x560), so no backend
needed a new swapchain. PrimedGun laid the laser ribbon flat in the
controller's frame; here it turns about the ray to face the head, so it never
shows edge-on. The quads use straight alpha, as PrimedGun's did. The virtual
screen no longer leaves room for an in-eye panel, which nothing draws any more.

**Behaviour.** This is PrimedGun's `UpdateVrMenu`, `AdjustVrMenuSetting` and
`ActivateVrMenuSelection`:
- The row under the laser is selected.
- The trigger or A clicks. A numeric row's value box steps the value down on
  its left half and up on its right half.
- Resets, EXIT GAME and the save-state actions need a second click within six
  seconds.
- Every opening starts on LAYOUT.
- CALIBRATION and CONTROL turn pages with PREVIOUS and NEXT buttons under the
  rows, with the page number between them. This replaces PrimedGun's PAGE row
  at the top of the list (the user's request, after the first headset test).
- "LONGER HELD PRESS FOR VR MENU" (one second) and "MENU REQUIRES HAND NEAR
  HEAD" (the visor gesture's zone) work as in PrimedGun.

Changes apply live. SAVE SETTINGS writes `port_settings.ini` now; otherwise the
file is written whenever the port next saves. EXIT GAME saves and quits the way
the F1 overlay's Exit game does.

**Port mappings.**
- STATES drives `PortSaveState`. It shows 8 slots, PrimedGun's Dolphin had 10.
- LOAD NEWEST loads the slot saved last. SAVE OLDEST saves into the first empty
  slot, else the one saved longest ago.
- TEXTURES applies the launcher's cannon slots (`launcher/core/cannon_textures`,
  now linked into the game) and reloads the user texture pack.

Thirteen rows change settings the port saves but does not read yet. These are
the launcher's "not active yet" keys:
- Calibration tab: VISOR HELMET, HEIGHT PROMPT, FLOOR POSITION MARKER, HUD
  VERTICAL and HUD HORIZONTAL.
- Movement tab: LEFT STICK STRAFE, MOVEMENT DIRECTION, MOVEMENT DEADZONE,
  MOVEMENT SPEED, MOVEMENT ACCELERATION, AIR ACCELERATION and SNAP TURN ANGLE.

**Threads.**
- The XR pacing thread opens and closes the menu (`settings_panel::Controls`).
  It also places the menu and aims the laser (`OpenXRInput::PlaceMenu`), and
  publishes the hit point with a click counter, so a click survives a
  game-thread stall.
- The game thread runs the menu from `CGraphics::EndScene`
  (`PortVr::VrMenuUpdate`, `platform/vr/vr_menu.cpp`). It rasterises the image
  only when its text changes, then hands it over with
  `aurora_set_stereo_panel_image`.
- Aurora's frame worker uploads the image into the panel layer's image
  (`stereo_overlay::layer_source`), swizzled for a BGRA swapchain.

**Settings and tests.**
- The menu's own settings (`vr_vr_overlays_enabled`, `vr_vr_menu_floating`,
  `vr_vr_menu_hold_left_stick` and `vr_vr_menu_requires_head_zone`) lost their
  "not active yet" tag.
- They are also in the F1 VR tab, with "Show it in the headset now".
- `port_vr_menu_tests` covers the image's pixels and byte order, the hit boxes,
  the two-press actions, the panel, laser and dot poses, and the open/close
  rules.

**Headset result** (Quest 3, direct presentation, 32-minute session): the user
confirmed the look, the pointing, live changes and the game's input while the
menu is open. The log shows `OpenXR VR menu layer ready` and no menu errors.
The PC (D3D12) path is not tested in a headset yet.

## VR: the PrimedGun launcher on the Quest (2026-10-06)

The Quest APK now opens on PrimedGun's launcher, a 2D Horizon OS panel. The
game, `PrimedGunVrActivity`, is no longer in the library; the panel's Play
starts it. The panel is PrimedGun's Quest launcher (its Kotlin rows, layouts,
palette and strings) with the PC launcher's tabs and keys: Setup, Controller,
Calibration, Cannon Textures, Layout, Port Config and About. Settings the game
saves but does not read yet carry the same "not active yet" tag as on the PC.

**One core for both launchers.** `launcher/jni` builds `libprimedgun_launcher.so`
(`-DMP_BUILD_QUEST_LAUNCHER=ON`) from `launcher/core`. The panel therefore edits
`port_settings.ini` with the PC launcher's key table, `SettingsModel` and
line-preserving file editor. It applies cannon slots and reads `PrimedGun.ini`
with the same code. Only strings, numbers and arrays cross JNI. The library has
no SDL or Aurora.

**Game process.** The game keeps its own `:game` process.
- Play saves pending edits, re-applies a cannon slot whose files went missing,
  and starts the game.
- The tabs lock while the process lives, because the game rewrites the whole
  file as it exits. The file is read again once the process is gone.
- Stop sends a package-scoped broadcast that finishes the game, as Quit does.
  Pressing Stop again after 10 s ends the process.
- `last_error.txt` (why a start failed) is shown when the panel returns.

**Select Game** copies the picked image to `<user>/disc.iso` in a `dataSync`
foreground service. The game process cannot open the picker's document.
- The PC launcher's disc check runs on the image's first 0x8008 bytes before
  the copy. The extension is lower-cased, and a WBFS image is known by its
  magic. A WBFS image is checked by the header copy of disc slot 0, which sits
  at its second hard-disk sector.
- The copy goes to `disc.iso.part`, which must be as long as the provider says.
  It must also pass the game's own check (`QuestStorage.checkDisc`) before it
  replaces the disc in use.
- A copy's result is kept until the Setup tab has shown it.
- A `.part` left by a killed process is removed at the next start.

**Memory card transfer.** On a headset the player picks a card (`.raw`, `.gcp`,
`.gci`), a `PrimedGun.ini`, or the zip that PrimedGun's Export User Data writes.
- **From a zip:** the raw card comes first (`MemoryCardA.USA[.<blocks>].raw`), as
  the PC search ranks it. The GCI folder is used only when it holds Metroid
  Prime saves. Entry names without the UTF-8 flag are read as Latin-1.
- **The hand-over:** the panel cannot read a raw card, because `port_gci.cpp`
  uses Aurora's card code, which is built on SDL.
  - The panel copies a set into `primedgun/pending_import.tmp` and renames it
    to `pending_import`.
  - At boot, before the card mounts, `PortGci::ImportPending` claims the folder
    by renaming it to `pending_import.claimed`. The import reads only that
    claimed set. A set staged meanwhile waits for the next start.
  - The `.gci` files form one save set, in any case of extension; each raw image
    is one set too.
  - An imported claim is renamed to `.done` before it is deleted. A failed
    cleanup therefore never re-imports old saves over newer progress. A claim
    left by a crash is taken up again.
  - The import writes `primedgun/import_report.txt`, which the panel shows.
- **Staging** runs at process level, like the disc copy. Play is refused until
  it ends.
- **Old settings** become unsaved edits, as on the PC. They are saved at once
  if the panel is no longer in front.

**Port Config on the Quest** leaves out VR on/off, the mirror, fullscreen and
VSync. It adds the refresh rate, the performance level, passthrough,
foveation, and the renderer switches the PC reaches through F1 (multiview,
direct presentation, pipelined rendering, indexed vertices, `[xr-diag]`).

**Game fix found on the way.** `vr_passthrough` never reached the OpenXR session.
Its copy was taken when the session object was built, which can be before the
file's line is read, and nothing called `OpenXRSetPassthrough`.
`PushVrSettingsToAurora` now passes it on with the refresh rate and lean-back.

Validation:
- **Review.** Three rounds of review, each finding checked by an independent
  skeptic. They found 26 defects, all fixed here: a recursion crash in the
  Setup tab, the passthrough setting, the zip card priority, a disc copy that
  could replace a good disc, the hand-over's atomicity and others.
- **PC tests.** build/vr passes 41/41 port tests, with new cases for the
  hand-over (A and B together, `.GCI`, an unfinished claim) and for WBFS.
- **Core tests on the headset.** `port_launcher_tests`, cross-compiled with the
  NDK, passes on a Quest 3.
- **Quest 3, driven over adb.**
  - All seven tabs open.
  - The "not active yet" tags match the key table.
  - Save writes only the changed key into the VR block.
  - Cannon Slot 1 and Default apply.
  - Play then boots the game, which imports a staged card through the claim.
  - The panel brought forward is locked; Stop ends the game in about 1 s and the
    panel unlocks.
- **Not tested on the headset.** Select Game and Transfer open the system file
  picker, which adb cannot drive.

## Renderer: native vertex input experiment (2026-10-06)

Native vertex input makes supported resident world geometry use the GPU's
vertex-input stage instead of loading and decoding its attributes from a
storage buffer in the vertex shader. It is on by default for play testing;
`MP_NATIVE_VERTICES=0` (also a Quest launch extra) keeps the storage path.
Wider-area and Quest 2 validation are still pending.

The geometry cache converts numeric components to little-endian once on a
miss. Attribute offsets, aligned record stride, absolute indices, surface
batching and draw order stay the same. Native float/integer formats and RGBA8
colours preserve the existing fixed-point scaling. Matrix-index bytes are
extracted from aligned integer attributes. Unsupported layouts (including NBT
and packed colours) retain the resident storage path; dynamic geometry, lines
and the map batch retain their existing paths. Cache keys distinguish the two
encodings. Mono, per-eye and multiview pipelines all support native input.

Validation: Windows and Quest builds, 235 FIFO tests, 19 renderer tests and
41 port tests pass. A desktop D3D12 Chozo run exercised 37-38 native draws per
steady frame without shader validation errors. The installed Quest 3 build
passed a visual check in both eyes at the saved Chozo plaza. Same-APK
off/on/off/on runs at scale 0.85 and 599 MHz averaged **9.60 ms -> 5.12 ms App
GPU time (46.6% less)**, with the same 715 draws (600 world); 515 world draws
used native input. Vertex instructions fell about 52% and the reported vertex
fetch stall metric fell from 67.8% to 33.3%. All runs sustained 72 Hz.
Detailed logs, matched CPU/memory-clock subsets and screenshots are described
in `docs/CHOZO_PERFORMANCE.md`. Quest 2 and area transitions remain untested.
Temporary device overrides were restored.

## VR: fixed foveated rendering on the Quest (2026-10-06)

The Quest's GPU shades every pixel of each eye at full rate although the
headset's lenses blur the periphery. Wiicompiled VR shipped fixed foveated
rendering for the same renderer, and that design is ported here.

**Not `XR_FB_foveation`.** The runtime's density maps only shape render passes
that draw into its swapchain images, and the eyes are drawn into Aurora's own
targets and then blitted (direct presentation) or copied (AHardwareBuffer
bridge) into the swapchain. So the map has to go on Aurora's eye passes, which
stock Dawn cannot do.

- **Dawn patch** (`quest/dawn/aurora_fdm.{h,inc}`, applied by `apply.py`;
  `extern/aurora/include/aurora/dawn_fdm_abi.h` is the C ABI, version 2):
  `VK_EXT_fragment_density_map` on Dawn's dynamic rendering path. Aurora
  uploads an immutable RG8 map (raw `VkImage`, `FRAGMENT_DENSITY_MAP_OPTIMAL`,
  read by the driver on the CPU when the pass is recorded, so it is usable only
  once its upload has completed) and binds it to a texture view;
  `RecordBeginDynamicRenderPass` chains the bound map into every pass whose
  first colour attachment is that view. The extension is requested before the
  device is created and then flags every render pipeline
  (`VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT`), so
  the choice is made per launch and the first launch with it on recompiles the
  pipeline cache once. Version 2 adds layered maps: under multiview the eye
  pass renders both eyes into one two-layer array view, and Vulkan takes a
  density map with one layer per view (layer = view index).
- **Aurora**: `lib/webgpu/fdm.{hpp,cpp}` wraps the ABI (stubs on every other
  Dawn; the Quest build checks `AuroraDawnFdmVersion()` at runtime);
  `lib/gfx/foveation.hpp` builds the map (Wiicompiled's rings: Low full within
  30 degrees of the eye's forward direction and 2x2 beyond, Medium 25 / 40,
  High 18 / 34; densities 255, 127 and 63 so a half cannot round back to one
  pixel; each eye's map is centred on its own asymmetric frustum, towards the
  nose); `lib/gfx/stereo_foveation.{hpp,cpp}` owns the per-target state: a
  second, render-attachment-only view of the eye targets (an explicit
  descriptor, since Dawn hands back the same object for every default view),
  the bound map, and a key of size, level and the four tangents per eye in
  hundredths. A new map replaces the bound one only once uploaded, so a live
  level change never shows an unfoveated frame. `stereo_host::begin_frame`
  prepares it after the targets; `stereo_seal_pass` makes the eye pass render
  through the foveated view (the multiview array view, or one per eye) while
  the copies taken from an eye keep the eye's own view
  (`StereoEyePass::copySourceView`). Every replayed eye pass of an immersive
  frame is foveated, splits included: Wiicompiled's "single render pass only"
  rule cannot be applied here because passes are sealed and handed to the
  render worker while the frame is still being recorded, and on Adreno a
  world-sized eye pass is binned anyway, so a load under a density map reads
  the same full-resolution tiles as one without. The virtual screen (menus,
  cinematics), the blit, the mirror and the EFB copies are never foveated.
  The targets release the maps before they are recreated (a binding keeps its
  view, and so the old texture, alive).
- **Setting** `vr_foveation` (`off`, `low`, `medium`, `high`; default `off`, see
  the measurement below; the desktop has no path for it): F1 VR tab
  "Foveated rendering (Quest)" (never disabled: a session started with it off
  has no maps, yet the level chosen is the next start's), launcher Port Config,
  `MP_FOVEATION=<level>` for one run (also an `am start --es` extra). Off to a
  level takes a restart; between levels and back to off it is live.
  `aurora_get_stereo_foveation` / `aurora_stereo_foveation_available` report it.
- **Logs**: "Fragment density maps: enabled, AxB to CxD pixels per texel, using
  32" at device creation (or why they are off), "Eye foveation low: 45x47
  density map x2, 32 pixels per texel, for the 1428x1496 eyes" per rebuild, and
  the `stereo frame:` statistics line counts the foveated passes. The `Fov=`
  field of Meta's `VrApi` logcat line reports the runtime's own foveation and
  stays 0: it is not an indicator here.
- **Measured** (Quest 3, Chozo plaza save state, `docs/CHOZO_PERFORMANCE.md`
  "Fixed foveated rendering"): the maps cost GPU time and save none. At
  599 MHz, App GPU 9.7 ms off, 9.9 with the device extension and pipeline flag
  alone, 10.2 with one shared map, 10.4 with the per-eye two-layer map; at
  render scale 1.25, 11.4 off against 12.1 / 12.2 / 11.9 for low / medium /
  high. Wiicompiled measured 8 to 22 % savings at 1.3 on Mario Kart; Prime's
  eye pass is bound by per-draw vertex fetch and binning, not fragment
  shading, and a map on a non-subsampled image still stores full tiles. The
  High screenshot (`build/foveation/quest_fov_high.jpg`) shows the coarse
  shading really applied (4x4 blocks on the visor frame and the walls, sharp
  centre), so the mechanism works; there is no pixel work to save here. Hence
  the default `off`; the setting stays for fill-heavy areas (water, fog, heat,
  snow), to be measured the same way. Diagnostics for that: `MP_FDM_DEVICE=1`
  gives the device the extension with the level off, `MP_FOVEATION_LAYERS=1`
  binds one shared map (each texel the finer of the two eyes') instead of the
  per-eye layers; both are accepted as launch extras.
- Tests: `extern/aurora/tests/foveation_test.cpp` (`foveation_tests`, the map
  generator at the port's eye size), the launcher's default row. Verified on
  the headset: the patched Dawn rebuilt (`quest/Build-Quest.ps1`), the log
  lines above, screenshots at low / high / off, no Vulkan or WebGPU errors.

## VR: the beam wheel's hover lights the HUD's beam box (2026-10-04)

PrimedGun showed its beam wheel (hold the weapon hand's B, point the cannon up,
right, down or left) on a panel of its own: four PNG icons floating in front of
the controller with an orange frame around the hovered one. The port has had
the wheel itself since the pad synthesis was lifted (`platform/vr/vr_pad.cpp`
tracks the hovered beam in `VrPadState::weapon_selected`), but nothing showed
the hover: the player released B blind and watched the HUD flash afterwards.

With source access the HUD's own beam menu does the job. `CHudVisorBeamMenu`
already draws the four beam boxes (`model_beamloz*` / `model_beamicon*`) and
recolours them every tick from the GuiColors tweak: the active grey (0.66) for
the current beam's icon, the inactive grey (0.56) for the others, and a
translucent dark blue for the current beam's lozenge. Those colours modulate
the models, so the game's own select flash (`kAP_SelectFlash`, alternating the
two greys on the pending beam) is the brightest the boxes ever get in retail.

- `PortVr::VrBeamWheelHoverBeam()` (`platform/vr/vr_view.cpp`) turns the pad's
  hover into `CPlayerState::EBeamId` (the wheel counts PrimedGun's way, Power /
  Wave / Ice / Plasma; the game puts Ice before Wave), or -1 when the wheel is
  closed, nothing is hovered, the controllers are not in gameplay, or the
  setting is off.
- `CSamusHud::UpdateVisorAndBeamMenus` hands it to the beam menu
  (`PortVrSetHighlight`), and `CHudVisorBeamMenu::Update` sets that item's icon
  and lozenge to white after the phase's colours, so the hovered box reads as a
  clear step brighter than everything else, selected beam included. A beam the
  player does not own yet is left alone (its icon is invisible anyway). The
  highlight lasts exactly as long as the hover: every phase that follows
  rewrites the colours.
- Setting `vr_beam_wheel_hud_highlight` (default on): F1 VR tab, Controls
  (with a live "Beam wheel: open / hover: Ice" readout), launcher Controller
  tab, Reset Controller list, `tests/port_launcher.cpp` default check.

The first headset run showed the wheel itself inverted: aiming up lit (and
switched to) Ice, aiming right Plasma. PrimedGun's `PrimedGunRollFreeQuat`
levels the panel with right = forward x up = (-fz, 0, fx); the port's copy in
`vr_pad.cpp` had (fz, 0, -fx), the left vector. With up = right x forward that
basis is the level one rolled 180 degrees about the aim, so the frozen panel
was upside down: both of its axes, the hand-travel fallback's too, read
negated, and its centre sat 5.5 cm below the aim instead of above. The aim ray
itself was right, which is why nothing looked broken until the HUD showed the
hover.

- The wheel's maths moved to the header-only
  `platform/include/vr/vr_beam_wheel.h` (levelled frame, panel, ray or travel
  measure, four-way pick, C-stick direction, `EBeamId` order), with the sign
  fixed. `vr_pad.cpp` and `VrBeamWheelHoverBeam` use it.
- `tests/port_vr_beam_wheel.cpp` (`port_vr_beam_wheel_tests`) turns a
  simulated controller up, right, down and left from six facings, pitches and
  rolls and expects Power, Wave, Ice and Plasma, and checks the levelled frame,
  the travel fallback and the mappings. With the old sign it fails on its
  first check.

Confirmed in the headset: the wheel picks the box it points at and the HUD lights it. build/vr and build/nooxr
pass 39/39.

# Port notes

Open follow-ups and non-obvious facts for the port. Closed items are removed; see
git history. Build, run and test instructions are in `docs/NATIVE_PORT.md`.

## Open follow-ups

### Touch overlay: a short tap can be missed

SDL's virtual joystick is state-sampling, not event-queueing: a press and release
that both land between two updates leave only the release, so a quick tap on A or
Start can do nothing, mostly while a frame is stalled. Fixing it means latching a
press until the game has sampled it, and releasing the latch on an update the port
does not control. Documented in `platform/include/touch_pad.h`. Also: the cached
virtual-pad pointer would not survive a full `SDL_Quit()`, so same-process activity
recreation would need explicit close/detach/reset (not reachable today:
`SDLActivity` exits the process on a second creation).

### Prompt art coverage

- `0xe14dc493` (yellow "C" badge, a C-stick prompt) is in `platform/port_prompts.cpp`
  but no reachable screen has been seen drawing it; not the pause menu, map or HUD.
- The front end's B prompt and the HUD hint-memo prompt have not been photographed,
  and nothing prompt-related was checked on a device.
- Finding a texture hash: `MP_DUMP_TEXTURES=1` dumps only textures that have no
  replacement yet and writes the images, so a candidate can be looked at. Deleting a
  table row does not remove the static per-device replacement files in `textures/`
  (`PortTextures` registers them by filename); delete the files too.

### Archipelago

- The real world is `UltiNaruto/MetroidAPrime` (the 404 on 2026-09-27 came from a
  misspelled `MetroidAPPrime`; the repository is reachable). The location join is
  still verified only against `tools/ap-world-fixture/Locations.py` (retail's 100
  locations, synthetic AP ids from 50310000); map the real table with
  `tools/make_ap_config.py --strict`.
- `MP_AP_RESET_STATE=1` is the manual way to discard progress for a new game or an
  older save on the same slot and seed (the card exposes no save identity).
- `MP_RANDO_DUMP` takes precedence over `MP_RANDO_SEED` and returns before placement,
  so setting both looks like the seed was ignored. A seed run wants
  `MP_RANDO_SWEEP=1` and no `MP_RANDO_DUMP`.

### Validation gaps

- Windows runtime and full-game traversal were never validated beyond the CI
  startup check. Full-game ThreadSanitizer is blocked by reports in uninstrumented
  GLib/libdbus/nod startup code; the 205 GPU-free FIFO/GX tests pass under TSan.
- Retail save compatibility (MSB-first bit fields, big-endian CRC, legacy native CRC
  accepted) has had no directed round trip with a retail GameCube save.
- A Windows CI note: the port's own tests that need an unloaded process re-run the
  test binary (`fork`/`execv`, `CreateProcessA`); winsock2.h needs `NOMINMAX` first.
- A full eight-world sweep (~37000 frames) can end in
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` loading the front end's first texture. The dump is
  already complete by then, so it is cosmetic for the sweep.
- Capture limits: the screenshot path (`F12`, console `shot`, via
  `aurora::request_screenshot`) does not include Aurora's ImGui layer, so the F1
  overlay cannot be verified from a capture; HUD memos are captured normally.

### Packaging

- Flathub: screenshots of the running game would show Nintendo's game, so none can
  ship; the app id is already `io.github.odrannnn.metroidprimeport`.
- The licence (`LICENSE` MIT over this project's own work; `NOTICE` excludes `src/`
  and `include/`) does not answer what may be done with a working copy of the
  decompiled game code; that remains a question for the copyright holder.

## Non-obvious facts

- **Audit of what a package contains**: no `.iso`/`.gcm`/`.wbfs`; the APK carries 162
  texture assets derived from Kenney's CC0 Input Prompts (attribution in
  `tools/prompt_icons/README.md`) and the third-party licence texts under `assets/`.
- **`OSDisableInterrupts` is a no-op on PC.** Anything the guest serialised with it
  needs a real mutex or ordering (DVD/ARAM transfer counters, `CDSPStream`
  `xec_readsPending`: publish state before starting an async read, drop completions
  for reads the stream no longer owns).
- **Plain `char` is unsigned on ARM**, so `-1` sentinels read back as 255. CMake passes
  `-fsigned-char` (`mp_signed_char`) and `platform/compat.h` asserts it.
- **`AIGetDMAStartAddr` truncates to 32 bits**; use the port's 64-bit accessor
  (`platform/ai_dma.cpp`). The AI DMA callback runs on the main thread because the
  guest mixer is not thread-safe. `MP_DISABLE_AI_AUDIO=1` isolates this path.
- **Aurora defers ARQ completion callbacks until `ARQPoll`**; loops that spin on a DMA
  token (`CARAMToken::UpdateAllDMAs`) must pump it.
- **`GXSetDrawSync`/`GXReadDrawSync` are real FIFO-ordered tokens.** The skinned-model
  workspace is freed when its token reads back, so an echoing shim corrupts draws.
- **Array sizes and endianness**: `GXSetArray` needs real byte sizes (0 draws nothing);
  runtime-generated arrays are host-native (`le=true`), and `ClearArray` is needed
  when a workspace pointer is reused each frame.
- **AGSC sample directory**: the disc form ends in a 4-byte `0xFFFFFFFF` terminator,
  so the ADPCM info blocks start at `(count - 1) * entrySize + 4`; `extraData` offsets
  are rebased onto the larger native `SDIR_DATA`. MusyX 2.0.0's `sndPopGroup` can
  leave voices referencing a popped group's samples, so group buffers live exactly
  as long as the group is pushed (`f2888a72`).
- **Wayland**: the game EFB stays at its configured 640x480 with
  `VISetFrameBufferScale(1)` and Aurora scales to the swapchain (otherwise the title
  background vanishes at fractional scales); compositor vsync is off in favour of an
  absolute 60 Hz deadline. SDL's Wayland backend can hang in `SDL_ShowWindow`
  (libdecor); see `docs/NATIVE_PORT.md`. Unattended runs on a Wayland session need
  `DISPLAY`, `XAUTHORITY` and `SDL_VIDEODRIVER=x11`.
- **Lock-on** (`CPlayer::WithinOrbitScreenBox`/`Ellipse`) compares screen position
  against fixed 640x480 tweak coordinates; they are scaled by the viewport size,
  otherwise the zone sits left of the reticle in widescreen.
- **Aspect** (`MP_ASPECT=4:3|16:9|window`): the game recomputes the render-mode width
  each frame (`CGraphics::PortResizeFrameBuffer`) and refreshes `CCameraManager`'s
  aspect; `AURORA_VIEWPORT_FIT` letterboxes instead of stretching.
- **Frame pacing**: the simulation is fixed-step at 60 Hz; uncapped (`F10`) only
  raises the presentation rate. The reported presented rate and throughput (wait
  excluded) differ at the cap and agree uncapped.
- **HD textures** (`MP_TEXTURES=<dir>`): Aurora's `tex1_<w>x<h>[_m]_<texhash>[_<tluthash>]_<format>.dds|.png`
  convention, `$` wildcards allowed. Dolphin packs use the same layout and
  `XXH64(data, size, 0)`, but the hashed size and TLUT handling are unconfirmed against
  a real pack.
- **`assets/initial_pipeline_cache.db`** holds machine-independent pipeline
  descriptions, copied beside the executable and merged into each user's cache.
- **Debug flags**: `MP_FAST_BOOT=1` skips the front end into a new game, so a
  Continue walk (`MP_SMOKE_CONTINUE`) needs it off. `MP_SKIP_CUTSCENES=1` fast-forwards
  cutscenes without a skip object (the frigate opening has none).
  `MP_SMOKE_SCRIPT`, `MP_SMOKE_SAVE` and the other smoke drivers are in
  `docs/NATIVE_PORT.md`.
- **Port hooks run from `CStateManager::Update`**, which only runs in `kSMT_InGame`;
  anything that must act while a menu is up has to run from `PortSmokeFrame` (main
  loop).
- **Android memory card path**: `CARDInit` fills an empty card path from
  `MP_USER_PATH` (unset on Android) or the working directory (not writable there).
  The port falls back to `SDL_GetAndroidInternalStoragePath()` and logs
  `memory card: storing under <path>`; start any device investigation of a card that
  the front end cannot identify from that line.
- **Android release signing**: release builds refuse the debug key unless asked
  (`tools/android_apk.sh` opts in with a stderr note unless `--strict-signing`);
  naming no signing config at all produces an uninstallable `app-release-unsigned.apk`.
- **A green Gradle build proves nothing by itself**: it can report success without
  compiling the C++. Check the object file postdates the edit.
- **Saves**: Prime saves at a save station (no save in the pause menu).
  `CARDCreate` alone leaves an 8192-byte zero file with CRC 0, which the game rightly
  calls corrupt; that is not a port bug.
- **Licensing**: Aurora and the vendored MusyX snapshot are MIT; the decompiled game
  and its assets stay Nintendo's, so ship no assets and require the user's disc. The
  earlier GPL recomp path (DolRecomp / ModernGekko) is kept separately and not linked.

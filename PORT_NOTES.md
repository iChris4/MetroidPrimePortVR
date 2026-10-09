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
  asks for, at ~2249 background compiles per start. A seed recorded on the
  headset can go in `quest/assets/initial_pipeline_cache.db`, which ships
  when present; the activity deletes a stale unpacked seed. No pointer
  capture on the Quest (a98aa72c is for phones' navigation bars).
- **Update check.** Upstream's asks Odrannnn/MetroidPrimePort about
  upstream's versionName, so it would point PrimedGun players at the flat
  port: it is off (ApplyUpdateCheck), and F1's Updates section links to
  PrimedGun's releases. The check itself stays in the tree for small merges.
- **Version.** The generated port_build_info.h also defines
  `MP_PRIMEDGUN_VERSION`, read from launcher/CMakeLists.txt's
  `set(PRIMEDGUN_LAUNCHER_VERSION "...")` (keep that form). --version, the
  log header and F1 About show "PrimedGun x (native port y), build z";
  MP_BUILD_VERSION stays upstream's.
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
- `CAnimData`: the constructor and `SetModel` accumulate `x108_aabb`, the
  fallback bounding box of a character with no per-animation boxes, from the
  same raw position array. Same swap (`PortReadBigVector`).
- Since upstream's NBT normals (ff4aa620, 7ecee577: N, B, T and sometimes a
  second B, T per entry), the swap reads each vertex's N at `NormalVecs()`
  vectors per entry, and `nativeNormals` keeps one vector per vertex.
  Upstream's version of this constructor still averages the raw big-endian
  floats: keep ours on merges.

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

- The real world (`UltiNaruto/MetroidAPPrime`) answered 404 on 2026-09-27, so the
  location join is verified only against `tools/ap-world-fixture/Locations.py`
  (retail's 100 locations, synthetic AP ids from 50310000). Re-check that
  repository; map the real table with `tools/make_ap_config.py --strict`.
- `platform/port_json.*` is a general JSON parser while `platform/port_randomizer.cpp`
  still has its own seed parser; folding it onto `PortJson` would leave one.
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

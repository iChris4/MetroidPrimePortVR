# Upstream PR #10 integration for PrimedGun VR

This testing branch merges iChris4/MetroidPrimePortVR PR #10 at upstream commit
`1ced107e0c8dcef60151ab1744141f9de00a3349` into the VR tree. It includes the
previous 0.29 integration and advances the native port to 0.30.0. The OpenXR
release branch and release version are unchanged.

## Compatibility decisions

- Retain the VR launchers, Quest package and signing setup, settings hooks,
  input mappings, roomscale stepping, camera transitions, tracked weapon and
  left-hand mirroring from the OpenXR branch.
- Adopt upstream's particle, Remastered import, gameplay and memory-management
  improvements, while retaining headset-aware particle rendering.
- Keep the VR pipeline seed and stereo selection alongside upstream's lazy
  pipeline compilation and eviction settings.
- Preserve VR's 10 MiB vertex ceiling at scale 1 and report the actual selected
  buffer scale independently of that minimum.
- Recreate multiview uniform bindings when upstream grows the uniform buffer.
- Make stereo uniforms and cached-geometry uploads work with transient staging
  spills. Relocate a mono uniform with its multiview clip block when necessary
  so the shader still finds adjacent data.
- Fall back from direct FIFO draw merging to ordinary spill-aware staging when
  fixed staging memory is full; keep the fast merge path when its ranges fit.
- Give uniform inspection tests explicit staging capacity, and cover storage
  spill limits and multiview clip adjacency with regression tests.

## Validation

- Windows x64 native game and Qt launcher built with OpenXR enabled.
- All 133 native-port checks and all 483 Aurora renderer checks passed.
- No unresolved Git conflicts or whitespace errors.
- Quest has not been rebuilt for this integration. In-headset testing remains
  necessary to assess camera comfort, visuals and runtime performance.

The local testing runtime uses its own copy of the existing beta.1 settings and
saves. It does not replace the alpha or beta runtime folders.

# Remastered particle material format

`VMAT` is a port-only particle property: `CNST`, a big-endian byte length, then
the blob below. All words and IEEE-754 floats are big-endian.

## VMAT v2 / v3

The v2 layout is unchanged:

- Header: `version`, `features`, `blend`, `textureCount` (four u32s).
- Each texture (11 words): id, UV set, wrap S/T, linear, columns, rows, layers,
  warped, warp U/V scales (the last two are floats).
- Seven signed texture slots: color, opacity, ramp, ramp2, threshold, indirect,
  palette; then two signed ramp rows and the signed add row.
- Eleven sources, each `{signed row, signed component, float default}`:
  erosion, thrX/Y/W, fresnelX/Y, fadeX/Y, indexScale/Offset/Row.
- Float modulate, float depthSoften, u32 spriteCenter.

Only shader `e899b1df` emits **v3**, with that exact v2 layout followed by
`float opacityGain = CCH0.z`, `float opacityBias = CCH0.w`. The reader accepts
v2 with neutral defaults `(1, 0)` and v3 with exactly two extra words. The blob
length is `16 + 44 * textureCount + 184` for v2, eight bytes longer for v3.
Unknown versions, invalid header bounds and wrong lengths keep the existing
absent-material/retail fallback. Other recipes still emit v2, including their
existing identity-CCH0 guards where applicable.

## VSMT and opacity

`VSMT` entries remain `{target, kind, element}` with 21 runtime target slots:

- 0..10: original source defaults, in the order above (unchanged).
- 11..18: four textures' U/V warp scales (unchanged).
- 19: opacity gain; 20: opacity bias.

For `e899b1df`, SMTR CCH0 components 2/3 map to 19/20. Components 0/1 retain
their texture-warp targets. Evaluated overrides do not replace the serialized
MATI defaults. The model-opacity selection copies the fade angle sources to
the Fresnel angle sources but preserves gain/bias.

Both Aurora opacity forms evaluate `gain * clamp(smooth3(f) - bias, 0, 1)`,
with gain **outside** the clamp. `OpacityFade` uses the existing normal-free
`f`, `OpacityFresnel` the existing view-normal `f`; identity `(1, 0)` preserves
the previous formulas. `misc2.y/z` carry gain/bias in the existing uniform
vec4, without changing its size or layout. No RGB, blending, depth or exposure
policy changes accompany this extension. Import stage effects is 29; old
imports require regeneration to receive these new bytes.

## Queen barrier

The Parasite Queen room's barrier is ten effects around five platforms: each platform's
refraction (VMAT feature bit 29, `VFX_BarrierShield`, MATI `6429c234`) and its glow (bit 30,
`VFX_BarrierGlow`, MATI `ba0acbd6`). They are staged as one roomfx v4 group and activated
all-or-none: the barrier's own platform collision, motion and rotation stay retail's, and only
the visual rings move to the port's effects. A missing or failed member (asset, texture token,
generation change, or a PART that fails the barrier validation - feature bit, ordinary 2-D maps,
VMSH v2 with the mesh colour, VPRM's CCH0 row, XFMD 3, the material's real PVRT variables and
VSMT at 32/33) falls back to the retail rings for the whole group, never part of it. A
save-state restore invalidates the group until the next area load. The conversion needs a
re-import (models 19, effects 104, rooms 35); an older import has no converted barrier effects
and keeps the retail rings. The shaders and their CPU feed are covered by their own tests
against converted PARTs; no rendered frame is claimed as proof for them.

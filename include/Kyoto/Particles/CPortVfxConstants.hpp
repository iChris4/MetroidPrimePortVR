#pragma once

// VSMT targets 0..10 are the original sources; 11..18 are texture warp scales.
// Keep their numbering stable when extending the material format.
constexpr unsigned kPortVfxOpacityGainTarget = 19;
constexpr unsigned kPortVfxOpacityBiasTarget = 20;
constexpr unsigned kPortVfxVsmtCount = 21;
// VPRM's CCHn uniforms: VSMT target kPortVfxParamTarget + 4 n + component, outside the 32-bit mask
// PortVfxEvalVsmt fills (it skips them); PortVfxEvalParams applies them.
constexpr unsigned kPortVfxParamTarget = 32;
constexpr unsigned kPortVfxParamCount = 7;

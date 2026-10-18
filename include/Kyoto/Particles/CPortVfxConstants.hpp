#pragma once

// VSMT targets 0..10 are the original sources; 11..18 are texture warp scales.
// Keep their numbering stable when extending the material format.
constexpr unsigned kPortVfxOpacityGainTarget = 19;
constexpr unsigned kPortVfxOpacityBiasTarget = 20;
constexpr unsigned kPortVfxVsmtCount = 21;

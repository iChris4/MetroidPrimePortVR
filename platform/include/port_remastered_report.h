#pragma once

// The Remastered import's converter reports: every decision the converters make,
// written into the mod as reports/materials.tsv, reports/effects.tsv and
// reports/summary.txt (docs/DEBUGGING.md "Converter reports"). Rows are plain
// text, sorted, so two imports diff cleanly. Nothing here needs a disc.

#include <cstdint>
#include <string>
#include <vector>

namespace PortRemastered {

// What the converter decided for one output material of a CMDL.
struct MaterialDecision {
  std::string cmdl;         // the output model's id, 8 hex digits
  int index = 0;            // the output material's index in the CMDL
  std::string source;       // the Remastered model it came from (a label; may be empty)
  int sourceIndex = 0;      // its material index in that model
  std::string shader;       // the Remastered shader id (id8)
  std::string role;         // the lists/constants the shader is in ("inverse-exposure+gun-body"), or "-"
  uint32_t flags = 0;       // the Remastered material's feature flags
  std::string tag;          // the record the port reads: PBRM..PBR7, WRAP or TEV
  int kind = 0;             // the PBR shader class (0 standard)
  int mode = 0;             // PBR mode word: 1 unlit, 2 glow mask, 4 vertex tint, 8 colour-unlit
  std::string path;         // pbr | tev
  std::string pathReason;   // why that path
  std::string kindReason;   // which rule or list chose the kind and mode
  double emissive = 0.0;
  double backlight = 0.0;
  double strength = 0.0;    // the kind's strength
  double p[4] = {0, 0, 0, 0};
  std::string cube;         // the reflection cube's id (8 hex digits), or "-"
  std::string retail;       // the retail material it stands on: "blendSrc,blendDst 0xflags"
  std::string meshClass;    // the Remastered mesh class (bits2): 0 opaque, 1 blended, 2 cutout, 3 additive, or "-"
  std::string blend;        // the blend written: "src,dst"
  std::string classBlend;   // whether it is the class's own (opaque/cutout 1,0, blended 4,5, additive 4,1): yes, no or "-"
  std::string uv;           // texcoord attributes (c: a colour too), then each PBR map's slot: "2c:0,1,0,0"
};

// One row of the effects report: what became of one Remastered effect, and why. A GENP with
// several retail PARTs has a row each.
struct EffectReportRow {
  std::string genp;       // the effect's id (EffectGuidString of the stored id)
  uint32_t retail = 0;    // the retail PART it stands for; 0 when unpaired
  std::string result;     // imported | failed | unpaired | no-disc-part
  std::string method;     // carried-over, or the pairing rule (name, room-placement, ...); none if unpaired
  std::string reason;     // for a failure, why
  std::string kinds;      // the files written: "PART:1,SWHC:2"
  int dropped = 0;        // retail properties left out (droppedRetail)
  std::vector<std::string> droppedList;       // "FOURCC: why", every left-out property
  std::vector<std::string> approximatedList;  // values written as an approximation
};

// Header lines (no newline) and one formatted row each, tab separated. A tab or newline in
// a field becomes a space.
std::string MaterialReportHeader();
std::string FormatMaterialRow(const MaterialDecision& row);
std::string EffectReportHeader();
std::string FormatEffectRow(const EffectReportRow& row);

// Header, then the rows sorted and with duplicates dropped, each ending in a newline.
std::string JoinReport(const std::string& header, std::vector<std::string> rows);

// Splits a report's text back into its data rows (the header is dropped).
std::vector<std::string> ReportRows(const std::string& text);

// The summary.txt text: counts per tag, kind, path and reason for the materials, and per
// result, method and failure reason for the effects. Either text may be empty.
std::string SummarizeReports(const std::string& materials, const std::string& effects);

}  // namespace PortRemastered

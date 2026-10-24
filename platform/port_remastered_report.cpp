#include "port_remastered_report.h"
#include "port_strings.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

namespace PortRemastered {

namespace {

std::string Clean(std::string text) {
  for (char& c : text) {
    if (c == '\t' || c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  return text.empty() ? "-" : text;
}

std::string Num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4g", v);
  return buf;
}

using port::Hex8;

std::string Join(const std::vector<std::string>& items) {
  std::string out;
  for (const std::string& item : items) {
    out += (out.empty() ? "" : ";") + item;
  }
  return out;
}

using port::Split;

const char* const kMaterialColumns[] = {"cmdl",   "mat",      "source",    "srcmat",   "shader",  "role",
                                        "flags",  "tag",      "kind",      "mode",     "path",    "pathReason",
                                        "kindReason", "emissive", "backlight", "strength", "p0",   "p1",
                                        "p2",     "p3",       "cube",     "retail",  "class",  "blend",
                                        "classBlend", "uv"};
const char* const kEffectColumns[] = {"genp",    "retail",  "result",  "method",      "reason",
                                      "kinds",   "dropped", "droppedList", "approximated"};

template <size_t N>
std::string HeaderOf(const char* const (&names)[N]) {
  std::string out;
  for (size_t i = 0; i < N; ++i) {
    out += (i ? "\t" : "") + std::string(names[i]);
  }
  return out;
}

// Counts of one column's values, as "  value: n" lines under a title.
void Count(std::ostringstream& out, const char* title, const std::map<std::string, int>& counts) {
  out << title << "\n";
  for (const auto& [value, n] : counts) {
    out << "  " << value << ": " << n << "\n";
  }
}

int Column(const std::vector<std::string>& header, const char* name) {
  for (size_t i = 0; i < header.size(); ++i) {
    if (header[i] == name) {
      return int(i);
    }
  }
  return -1;
}

}  // namespace

std::string MaterialReportHeader() { return HeaderOf(kMaterialColumns); }

std::string FormatMaterialRow(const MaterialDecision& r) {
  char index[16];
  std::snprintf(index, sizeof(index), "%03d", r.index);
  char src[16];
  std::snprintf(src, sizeof(src), "%d", r.sourceIndex);
  char flags[16];
  std::snprintf(flags, sizeof(flags), "0x%X", r.flags);
  const std::vector<std::string> f = {Clean(r.cmdl),      index,
                                      Clean(r.source),    src,
                                      Clean(r.shader),    Clean(r.role),
                                      flags,              Clean(r.tag),
                                      std::to_string(r.kind), std::to_string(r.mode),
                                      Clean(r.path),      Clean(r.pathReason),
                                      Clean(r.kindReason), Num(r.emissive),
                                      Num(r.backlight),   Num(r.strength),
                                      Num(r.p[0]),        Num(r.p[1]),
                                      Num(r.p[2]),        Num(r.p[3]),
                                      Clean(r.cube),      Clean(r.retail),
                                      Clean(r.meshClass), Clean(r.blend),
                                      Clean(r.classBlend), Clean(r.uv)};
  std::string out;
  for (size_t i = 0; i < f.size(); ++i) {
    out += (i ? "\t" : "") + f[i];
  }
  return out;
}

std::string EffectReportHeader() { return HeaderOf(kEffectColumns); }

std::string FormatEffectRow(const EffectReportRow& r) {
  const std::vector<std::string> f = {Clean(r.genp),
                                      r.retail ? Hex8(r.retail) : "-",
                                      Clean(r.result),
                                      Clean(r.method),
                                      Clean(r.reason),
                                      Clean(r.kinds),
                                      std::to_string(r.dropped),
                                      Clean(Join(r.droppedList)),
                                      Clean(Join(r.approximatedList))};
  std::string out;
  for (size_t i = 0; i < f.size(); ++i) {
    out += (i ? "\t" : "") + f[i];
  }
  return out;
}

std::string JoinReport(const std::string& header, std::vector<std::string> rows) {
  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  std::string out = header + "\n";
  for (const std::string& row : rows) {
    out += row + "\n";
  }
  return out;
}

std::vector<std::string> ReportRows(const std::string& text) {
  std::vector<std::string> rows;
  std::istringstream in(text);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (first) {
      first = false;
      continue;
    }
    if (!line.empty()) {
      rows.push_back(line);
    }
  }
  return rows;
}

std::string SummarizeReports(const std::string& materials, const std::string& effects) {
  std::ostringstream out;
  {
    const std::vector<std::string> rows = ReportRows(materials);
    out << "materials: " << rows.size() << "\n";
    if (!rows.empty()) {
      const std::vector<std::string> header = Split(materials.substr(0, materials.find('\n')), '\t');
      std::map<std::string, int> tag, kind, path, pathReason, kindReason, role;
      const int cTag = Column(header, "tag"), cKind = Column(header, "kind"), cPath = Column(header, "path"),
                cPathReason = Column(header, "pathReason"), cKindReason = Column(header, "kindReason"),
                cRole = Column(header, "role");
      for (const std::string& row : rows) {
        const std::vector<std::string> f = Split(row, '\t');
        auto at = [&](int c) { return c >= 0 && size_t(c) < f.size() ? f[size_t(c)] : std::string("?"); };
        ++tag[at(cTag)];
        ++kind[at(cKind)];
        ++path[at(cPath)];
        ++pathReason[at(cPathReason)];
        ++kindReason[at(cKindReason)];
        ++role[at(cRole)];
      }
      Count(out, "by record tag:", tag);
      Count(out, "by PBR kind:", kind);
      Count(out, "by path:", path);
      Count(out, "by path reason:", pathReason);
      Count(out, "by kind reason:", kindReason);
      Count(out, "by shader role:", role);
    }
  }
  {
    const std::vector<std::string> rows = ReportRows(effects);
    out << "effects: " << rows.size() << "\n";
    if (!rows.empty()) {
      const std::vector<std::string> header = Split(effects.substr(0, effects.find('\n')), '\t');
      std::map<std::string, int> result, method, failure;
      const int cResult = Column(header, "result"), cMethod = Column(header, "method"),
                cReason = Column(header, "reason");
      int dropped = 0;
      const int cDropped = Column(header, "dropped");
      for (const std::string& row : rows) {
        const std::vector<std::string> f = Split(row, '\t');
        auto at = [&](int c) { return c >= 0 && size_t(c) < f.size() ? f[size_t(c)] : std::string("?"); };
        ++result[at(cResult)];
        ++method[at(cResult) + " via " + at(cMethod)];
        if (at(cResult) == "failed") {
          std::string reason = at(cReason);
          ++failure[reason.substr(0, reason.find(':'))];
        }
        dropped += std::atoi(at(cDropped).c_str());
      }
      Count(out, "by result:", result);
      Count(out, "by result and method:", method);
      Count(out, "failures by reason:", failure);
      out << "retail properties dropped: " << dropped << "\n";
    }
  }
  return out.str();
}

}  // namespace PortRemastered

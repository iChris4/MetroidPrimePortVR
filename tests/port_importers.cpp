#include "port_importers.h"
#include "port_test.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void Feed(const char* text, std::string& partial, std::vector<std::string>& lines) {
  PortImporters::SplitLines(text, std::strlen(text), partial, lines);
}

void TestLines() {
  std::string partial;
  std::vector<std::string> lines;
  Feed("one\ntw", partial, lines);
  CHECK_MSG(lines.size() == 1 && lines[0] == "one" && partial == "tw", "a line split across reads waits for its end");
  Feed("o\r\nthree\n", partial, lines);
  CHECK_MSG(lines.size() == 3 && lines[1] == "two" && lines[2] == "three" && partial.empty(), "CRLF ends one line");
  Feed("10%\r20%\r30%", partial, lines);
  CHECK_MSG(lines.size() == 3 && partial == "30%", "a carriage return rewrites the line");
  Feed("\n\n", partial, lines);
  CHECK_MSG(lines.size() == 5 && lines[3] == "30%" && lines[4].empty(), "an empty line is kept");
}

void TestCap() {
  std::string partial;
  std::vector<std::string> lines;
  std::string text;
  for (size_t i = 0; i < PortImporters::kMaxLines + 50; ++i) {
    text += std::to_string(i) + "\n";
  }
  PortImporters::SplitLines(text.data(), text.size(), partial, lines);
  CHECK_MSG(lines.size() == PortImporters::kMaxLines, "the oldest lines are dropped");
  CHECK_MSG(lines.front() == "50" && lines.back() == std::to_string(PortImporters::kMaxLines + 49),
            "the newest lines are the ones kept");
}
} // namespace

int main() {
  TestLines();
  TestCap();
  if (port_test::Failures() == 0) {
    std::puts("port_importers_tests: ok");
  }
  return PORT_TEST_RESULT();
}

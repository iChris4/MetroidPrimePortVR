#include "port_update_check.h"
#include "port_test.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
  using namespace PortUpdateCheck;

  REQUIRE(CompareVersions("0.18.0", "0.18.0") == 0);
  REQUIRE(CompareVersions("v0.18.0", "0.18.0") == 0);
  REQUIRE(CompareVersions("0.18", "0.18.0") == 0);
  REQUIRE(CompareVersions("0.19.0", "0.18.9") > 0);
  REQUIRE(CompareVersions("0.18.10", "0.18.9") > 0);
  REQUIRE(CompareVersions("1.0", "0.99.99") > 0);
  REQUIRE(CompareVersions("0.18.0", "0.18.1") < 0);
  REQUIRE(CompareVersions("", "0.1") < 0);
  REQUIRE(CompareVersions("0.19.0-rc1", "0.18.0") < 0);
  REQUIRE(CompareVersions("0.18.0", "nightly") > 0);
  REQUIRE(CompareVersions("1..2", "1.2") < 0);
  REQUIRE(CompareVersions("1.2.", "1.2") < 0);
  REQUIRE(CompareVersions("99999999999.0", "1.0") < 0);

  Release release;
  REQUIRE(ParseLatestRelease(R"({"tag_name":"v0.19.0","draft":false,"prerelease":false,)"
                           R"("html_url":"https://github.com/Odrannnn/MetroidPrimePort/releases/tag/v0.19.0"})",
                           release));
  REQUIRE(release.version == "0.19.0");
  REQUIRE(release.url == "https://github.com/Odrannnn/MetroidPrimePort/releases/tag/v0.19.0");
  REQUIRE(ParseLatestRelease(R"({"tag_name":"0.20.1","html_url":"https://evil.example/x"})", release));
  REQUIRE(release.version == "0.20.1");
  REQUIRE(release.url == "https://github.com/Odrannnn/MetroidPrimePort/releases/latest");
  REQUIRE(ParseLatestRelease(R"({"tag_name":"1.0","html_url":"https://github.com/Odrannnn/MetroidPrimePort/\u0001"})",
                           release));
  REQUIRE(release.url == "https://github.com/Odrannnn/MetroidPrimePort/releases/latest");
  REQUIRE(!ParseLatestRelease(R"({"tag_name":"latest"})", release));
  REQUIRE(!ParseLatestRelease(R"({"tag_name":"v1.0","draft":true})", release));
  REQUIRE(!ParseLatestRelease(R"({"message":"Not Found"})", release));
  REQUIRE(!ParseLatestRelease("not json", release));

  // A recent state file answers at once, without asking GitHub.
  const std::string state = std::string(argc > 1 ? argv[1] : ".") + "/update-check-test.txt";
  {
    std::ofstream out(state, std::ios::trunc);
    out << "checked=" << static_cast<long long>(std::time(nullptr)) << "\nlatest=0.99.0\n"
        << "url=https://github.com/Odrannnn/MetroidPrimePort/releases/tag/v0.99.0\n";
  }
  Configure(false, "0.18.0", state);
  REQUIRE(Status() == kStatus_Off);
  REQUIRE(Latest().version == "0.99.0");
  Configure(true, "0.18.0", state);
  REQUIRE(Status() == kStatus_Available);
  REQUIRE(Latest().url == "https://github.com/Odrannnn/MetroidPrimePort/releases/tag/v0.99.0");
  Configure(true, "0.99.0", state);
  REQUIRE(Status() == kStatus_UpToDate);
  Configure(true, "1.0.0", state);
  REQUIRE(Status() == kStatus_UpToDate);

  // MP_UPDATE_LIVE=1 asks GitHub for real.
  if (const char* live = std::getenv("MP_UPDATE_LIVE"); live != nullptr && live[0] == '1') {
    const std::string fresh = state + ".live";
    std::remove(fresh.c_str());
    Configure(true, "0.0.1", fresh);
    CheckNow();
    for (int i = 0; i < 300 && LastChecked() == 0 && Status() != kStatus_Failed; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("live: status %d, latest %s, url %s, error '%s'\n", static_cast<int>(Status()),
                Latest().version.c_str(), Latest().url.c_str(), LastError().c_str());
    REQUIRE(Status() == kStatus_Available);
  }
  std::remove(state.c_str());
  std::remove((state + ".live").c_str());
  return 0;
}

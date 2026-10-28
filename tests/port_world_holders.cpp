// The mod reload's lifecycle contract (platform/include/port_world_holders.h), with the two holders
// stubbed here so the test sees what the production Release() (platform/port_world_holders.cpp,
// compiled in) actually does: both holders, in order, before the PAKs reopen, on every reload.
#include "port_test.h"
#include "port_third_person.h"
#include "port_viewmodel.h"
#include "port_world_holders.h"

#include <string>
#include <vector>

namespace {

std::vector< std::string >* sSteps = nullptr;

void Step(const char* what) {
  if (sSteps != nullptr) {
    sSteps->push_back(what);
  }
}

} // namespace

// The holders Release() calls, stubbed (the real ones own game state).
namespace PortThirdPerson {
void Reset() { Step("third-person"); }
} // namespace PortThirdPerson
namespace PortViewModel {
void Hide() { Step("viewmodel"); }
} // namespace PortViewModel

namespace {

void ReopenPaks() { Step("reopen"); }

} // namespace

int main() {
  {
    std::vector< std::string > steps;
    sSteps = &steps;
    PortWorldHolders::ReloadMods(&PortWorldHolders::Release, &ReopenPaks);
    sSteps = nullptr;
    CHECK(steps.size() == 3);
    if (steps.size() == 3) {
      CHECK(steps[0] == "third-person");
      CHECK(steps[1] == "viewmodel");
      CHECK(steps[2] == "reopen");
    }
  }
  {
    // Every reload releases both again: a front end makes one per switch, and both rebuild.
    std::vector< std::string > steps;
    sSteps = &steps;
    PortWorldHolders::ReloadMods(&PortWorldHolders::Release, &ReopenPaks);
    PortWorldHolders::ReloadMods(&PortWorldHolders::Release, &ReopenPaks);
    sSteps = nullptr;
    CHECK(steps.size() == 6);
    if (steps.size() == 6) {
      CHECK(steps[0] == "third-person" && steps[1] == "viewmodel" && steps[2] == "reopen");
      CHECK(steps[3] == "third-person" && steps[4] == "viewmodel" && steps[5] == "reopen");
    }
  }
  return PORT_TEST_RESULT();
}

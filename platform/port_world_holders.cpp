// The port's own model holders; see port_world_holders.h.
#include "port_world_holders.h"

#include "port_third_person.h"
#include "port_viewmodel.h"

namespace PortWorldHolders {

void Release() {
  PortThirdPerson::Reset();
  PortViewModel::Hide();
}

} // namespace PortWorldHolders

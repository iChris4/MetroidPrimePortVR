// The third-person body's clip choice (platform/include/port_third_person_body.h).
#include "port_test.h"
#include "port_third_person_body.h"

using namespace PortThirdPersonBody;

int main() {
  CHECK(ChooseClip(0.f, 0.f, true).clip == Clip::Idle);
  CHECK(ChooseClip(0.2f, 0.1f, true).clip == Clip::Idle);
  CHECK(ChooseClip(2.f, 0.f, true).clip == Clip::SlowWalk);
  CHECK(ChooseClip(5.f, 0.f, true).clip == Clip::Walk);
  CHECK(ChooseClip(9.f, 0.f, true).clip == Clip::Run);
  CHECK(ChooseClip(-5.f, 0.f, true).clip == Clip::Backward);
  CHECK(ChooseClip(0.5f, 5.f, true).clip == Clip::Hold); // strafing
  CHECK(ChooseClip(9.f, 0.f, false).clip == Clip::Hold); // airborne
  CHECK(ChooseClip(2.f, 0.f, true).rate >= 0.5f && ChooseClip(30.f, 0.f, true).rate <= 1.6f);
  return 0;
}

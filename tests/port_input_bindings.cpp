#include "port_input_bindings.h"
#include "port_test.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>

using namespace PortInput;

namespace {

Input In(Device d, uint16_t code, int8_t dir = 0, uint8_t threshold = 50) { return {d, dir, threshold, code}; }

Binding Bind(Action a, std::initializer_list<Input> inputs) {
  Binding b;
  b.action = a;
  for (const Input& in : inputs)
    b.inputs[b.count++] = in;
  return b;
}

bool RoundTrips(const Input& in) {
  Input back;
  return InputFromText(InputToText(in), back) && back == in;
}

std::string FakeKeyName(int code) { return code == 44 ? "space" : code == 30 ? "1" : code == 31 ? "@" : ""; }
int FakeKeyParse(std::string_view name) {
  return name == "space" ? 44 : name == "1" ? 30 : name == "@" ? 31 : -1;
}

int ErrorLine(const char* text) {
  UserBindings out;
  std::string error;
  if (ParseUserBindings(text, out, &error))
    return 0;
  return std::atoi(error.c_str() + 5); // "line N: ..."
}

void TestInputText() {
  for (int d = int(Device::Key); d <= int(Device::Gyro); ++d)
    for (uint16_t code : {0, 1, 2})
      for (int8_t dir : {0, 1, -1}) {
        Device device = Device(d);
        if ((device == Device::MouseWheel || device == Device::MouseMotion) && code > 1)
          continue;
        bool directed = device == Device::MouseWheel || device == Device::MouseMotion ||
                        device == Device::PadAxis || device == Device::TouchAxis || device == Device::Gyro;
        REQUIRE(RoundTrips(In(device, code, directed ? dir : 0)));
        REQUIRE(RoundTrips(In(device, code, directed ? dir : 0, 75)));
      }
  for (uint16_t code = 0; code < kMouseButtonCount; ++code)
    REQUIRE(RoundTrips(In(Device::MouseButton, code)));
  for (uint16_t code = 0; code < kPadButtonCount; ++code)
    REQUIRE(RoundTrips(In(Device::PadButton, code)));
  for (uint16_t code = 0; code < kPadAxisCount; ++code)
    REQUIRE(RoundTrips(In(Device::PadAxis, code, -1)));
  REQUIRE(RoundTrips(In(Device::Key, kKeyCount - 1)));
  REQUIRE(RoundTrips(In(Device::Touch, kTouchCount - 1)));

  REQUIRE(InputToText(In(Device::PadButton, 9)) == "pad:leftshoulder");
  REQUIRE(InputToText(In(Device::PadButton, 16)) == "pad:paddle1");
  REQUIRE(InputToText(In(Device::PadButton, 30)) == "pad:30");
  REQUIRE(InputToText(In(Device::PadAxis, 5, 1)) == "axis:righttrigger+");
  REQUIRE(InputToText(In(Device::PadAxis, 2)) == "axis:rightx");
  REQUIRE(InputToText(In(Device::MouseButton, 8)) == "mouse:left");
  REQUIRE(InputToText(In(Device::MouseButton, 2)) == "mouse:aim:right");
  REQUIRE(InputToText(In(Device::MouseButton, 17)) == "mouse:menu:middle");
  REQUIRE(InputToText(In(Device::MouseButton, 28)) == "mouse:held:x2");
  REQUIRE(InputToText(In(Device::MouseWheel, 0, 1)) == "wheel:up");
  REQUIRE(InputToText(In(Device::MouseWheel, 1, -1)) == "wheel:left");
  REQUIRE(InputToText(In(Device::MouseMotion, 1)) == "motion:y");
  REQUIRE(InputToText(In(Device::Gyro, 1, -1)) == "gyro:yaw-");
  REQUIRE(InputToText(In(Device::TouchAxis, 3, 1, 20)) == "touchaxis:3+@20");
  REQUIRE(InputToText(In(Device::Key, 44)) == "key:44");

  Input in;
  REQUIRE(InputFromText("axis:lefttrigger+@30", in) && in == In(Device::PadAxis, 4, 1, 30));
  REQUIRE(InputFromText(" pad:a ", in) && in == In(Device::PadButton, 0));
  REQUIRE(!InputFromText("pad:a@101", in));
  REQUIRE(!InputFromText("pad:nope", in));
  REQUIRE(!InputFromText("pad:32", in));
  REQUIRE(!InputFromText("touch:64", in));
  REQUIRE(!InputFromText("key:space", in)); // no key names installed
  REQUIRE(!InputFromText("bogus:1", in));
  REQUIRE(!InputFromText("pad:", in));
  REQUIRE(!InputFromText("gyro:3", in));

  SetKeyNames(FakeKeyName, FakeKeyParse);
  REQUIRE(InputToText(In(Device::Key, 44)) == "key:space");
  REQUIRE(InputToText(In(Device::Key, 45)) == "key:45");
  REQUIRE(InputFromText("key:space", in) && in == In(Device::Key, 44));
  REQUIRE(InputFromText("key:45", in) && in == In(Device::Key, 45));
  REQUIRE(!InputFromText("key:enter", in));
  REQUIRE(InputFromText("key:1", in) && in == In(Device::Key, 30)); // the name wins
  REQUIRE(InputFromText("key:@", in) && in == In(Device::Key, 31));
  REQUIRE(InputFromText("key:@@20", in) && in == In(Device::Key, 31, 0, 20));
  REQUIRE(RoundTrips(In(Device::Key, 31)) && RoundTrips(In(Device::Key, 30, 0, 75)));
  SetKeyNames(nullptr, nullptr);

  REQUIRE(InputToText(In(Device::Touch, 3)) == "touch:a");
  REQUIRE(InputToText(In(Device::Touch, 28)) == "touch:tturbo");
  REQUIRE(InputToText(In(Device::Touch, 32)) == "touch:beam_shift");
  REQUIRE(InputToText(In(Device::Touch, 40)) == "touch:40");
  REQUIRE(InputFromText("touch:missile", in) && in == In(Device::Touch, 20));
  REQUIRE(InputFromText("touch:map_tap", in) && in == In(Device::Touch, 34));
  REQUIRE(InputFromText("touch:5", in) && in == In(Device::Touch, 5));
  REQUIRE(!InputFromText("touch:nope", in));
  for (uint16_t code = 0; code < kTouchCount; ++code)
    REQUIRE(RoundTrips(In(Device::Touch, code)));
}

void TestContexts() {
  REQUIRE(ContextsToText(kCtxAll) == "all");
  REQUIRE(ContextsToText(kCtxGameplay | kCtxMap) == "gameplay,map");
  REQUIRE(ContextsToText(kCtxLayer2) == "layer2");
  uint16_t c = 0;
  REQUIRE(ContextFromText("morphball", c) && c == kCtxMorphBall);
  REQUIRE(ContextFromText("all", c) && c == kCtxAll);
  REQUIRE(!ContextFromText("nowhere", c));
}

void TestRoundTrip() {
  UserBindings in;
  UserProfile base;
  base.name = "Default \"mine\" \\ here";
  base.unbind = {{Action::PadZ, Family::Pad}, {Action::Screenshot, Family::Keyboard}};
  Binding chord = Bind(Action::BeamWave, {In(Device::PadButton, 9), In(Device::PadButton, 14)});
  base.bindings.push_back(chord);
  Binding any = Bind(Action::SaveState, {In(Device::Key, 224), In(Device::Key, 22), In(Device::Key, 4)});
  any.anyOrder = true;
  any.contexts = kCtxGameplay | kCtxMorphBall;
  base.bindings.push_back(any);
  for (Trigger t : {Trigger::Tap, Trigger::Hold, Trigger::DoubleTap, Trigger::Toggle}) {
    Binding b = Bind(Action::PadA, {In(Device::MouseButton, 10)});
    b.trigger = t;
    b.tapMs = 150;
    b.holdMs = 400;
    b.doubleMs = 333;
    base.bindings.push_back(b);
  }
  Binding turbo = Bind(Action::PadA, {In(Device::Touch, 33)});
  turbo.turboHz = 15;
  turbo.contexts = kCtxLayer1;
  base.bindings.push_back(turbo);
  Binding look = Bind(Action::LookX, {In(Device::Gyro, 1)});
  look.scale = 2.37f;
  look.invert = true;
  base.bindings.push_back(look);
  Binding whole = Bind(Action::LookY, {In(Device::MouseMotion, 1)});
  whole.scale = 3.f;
  base.bindings.push_back(whole);
  in.profiles.push_back(base);

  UserProfile pad;
  pad.name = "DualSense";
  pad.match = "type:ps5";
  pad.replace = true;
  pad.chordWindowMs = 60;
  pad.bindings.push_back(Bind(Action::PadB, {In(Device::PadAxis, 4, 1, 25)}));
  in.profiles.push_back(pad);
  UserProfile empty;
  empty.name = "";
  empty.match = "guid:0300ab";
  in.profiles.push_back(empty);

  std::string text = SerializeUserBindings(in);
  UserBindings back;
  std::string error;
  REQUIRE(ParseUserBindings(text, back, &error));
  REQUIRE(back == in);
  REQUIRE(SerializeUserBindings(back) == text);
  // Defaults are left out.
  REQUIRE(text.find("tap_ms = 200") == std::string::npos);
  REQUIRE(text.find("contexts = [\"all\"]") == std::string::npos);
  REQUIRE(text.find("contexts = [\"gameplay\", \"morphball\"]") != std::string::npos);
  REQUIRE(text.find("scale = 3.0") != std::string::npos);

  // Hand-written forms: comments, trailing comma, integer scale, "all".
  const char* hand = "# mine\n"
                     "[[profile]]  # the default\n"
                     "name = \"x\"\n"
                     "unbind = []\n"
                     "\n"
                     "[[profile.bind]]\n"
                     "action = \"pad_a\"\n"
                     "inputs = [\"pad:a\", ]\n"
                     "scale = 2\n"
                     "contexts = [\"all\"]\n";
  REQUIRE(ParseUserBindings(hand, back, &error));
  REQUIRE(back.profiles.size() == 1 && back.profiles[0].bindings.size() == 1);
  REQUIRE(back.profiles[0].match.empty());
  REQUIRE(back.profiles[0].bindings[0].scale == 2.f);
  REQUIRE(back.profiles[0].bindings[0].contexts == kCtxAll);
  REQUIRE(ParseUserBindings("", back) && back.profiles.empty());
  REQUIRE(ParseUserBindings("\xEF\xBB\xBF[[profile]]\nname = \"bom\"\n", back, &error));
  REQUIRE(back.profiles.size() == 1 && back.profiles[0].name == "bom");
}

void TestErrors() {
  REQUIRE(ErrorLine("[[profile]]\nname = \"a\"\n[[profile.bind]]\naction = \"jump\"\ninputs = [\"pad:a\"]\n") == 4);
  REQUIRE(ErrorLine("[[profile]]\ncolour = \"red\"\n") == 2);
  REQUIRE(ErrorLine("[[profile]]\n[[profile.bind]]\naction = \"pad_a\"\ninputs = [\"pad:zz\"]\n") == 4);
  REQUIRE(ErrorLine("\n[[profile.bind]]\naction = \"pad_a\"\n") == 2);
  REQUIRE(ErrorLine("[[profile]]\n[[profile.bind]]\naction = \"pad_a\"\n"
                    "inputs = [\"pad:a\", \"pad:b\", \"pad:x\", \"pad:y\", \"pad:back\"]\n") == 4);
  REQUIRE(ErrorLine("[[profile]]\n[[profile.bind]]\naction = \"pad_a\"\ninputs = []\n") == 4);
  REQUIRE(ErrorLine("[[profile]]\n\n[[profile.bind]]\naction = \"pad_a\"\n") == 3);  // no inputs
  REQUIRE(ErrorLine("[[profile]]\nchord_window_ms = 70000\n") == 2);
  REQUIRE(ErrorLine("[[profile]]\nreplace = 1\n") == 2);
  REQUIRE(ErrorLine("[[profile]]\nname = \"open\n") == 2);
  REQUIRE(ErrorLine("[[profile]]\nunbind = [\"pad_a@mouse\"]\n") == 2);
  REQUIRE(ErrorLine("name = \"x\"\n") == 1);
  REQUIRE(ErrorLine("[controls]\n") == 1);
  const char* bind = "[[profile]]\n[[profile.bind]]\naction = \"pad_a\"\ninputs = [\"pad:a\"]\n";
  REQUIRE(ErrorLine((std::string(bind) + "scale = inf\n").c_str()) == 5);
  REQUIRE(ErrorLine((std::string(bind) + "scale = nan\n").c_str()) == 5);
  REQUIRE(ErrorLine((std::string(bind) + "scale = 1e300\n").c_str()) == 5);
  REQUIRE(ErrorLine((std::string(bind) + "scale = 101.0\n").c_str()) == 5);

  UserBindings out;
  out.profiles.emplace_back();
  REQUIRE(!ParseUserBindings("[[profile]]\nbad\n", out));
  REQUIRE(out.profiles.size() == 1); // left unchanged
}

void TestSelect() {
  UserBindings b;
  auto add = [&](const char* name, const char* match) {
    UserProfile p;
    p.name = name;
    p.match = match;
    b.profiles.push_back(p);
  };
  add("type", "type:PS5");
  add("base", "");
  add("guid", "guid:0300ABCD");
  add("base2", "");
  add("type2", "type:ps5");

  Selection s = Select(b, "0300abcd", "ps5");
  REQUIRE(s.base && s.base->name == "base");
  REQUIRE(s.pad && s.pad->name == "guid");
  s = Select(b, "ffff", "Ps5");
  REQUIRE(s.pad && s.pad->name == "type");
  s = Select(b, "", "");
  REQUIRE(s.base && s.base->name == "base" && !s.pad);
  s = Select(b, "", "xbox360");
  REQUIRE(!s.pad);
  REQUIRE(!Select({}, "a", "b").base);
}

void TestOverlay() {
  Profile base;
  base.bindings.push_back(Bind(Action::PadA, {In(Device::Key, 44)}));
  base.bindings.push_back(Bind(Action::PadA, {In(Device::PadButton, 0)}));
  base.bindings.push_back(Bind(Action::PadZ, {In(Device::PadButton, 10)}));
  base.bindings.push_back(Bind(Action::PadZ, {In(Device::Touch, 5)}));
  base.bindings.push_back(Bind(Action::PadB, {In(Device::PadButton, 1)}));
  // A chord counts by its trigger (last input): this one is a keyboard binding.
  base.bindings.push_back(Bind(Action::PadB, {In(Device::PadButton, 9), In(Device::Key, 5)}));

  UserProfile user;
  user.bindings.push_back(Bind(Action::PadA, {In(Device::PadButton, 9), In(Device::PadButton, 2)}));
  user.unbind.push_back({Action::PadZ, Family::Pad});
  user.bindings.push_back(Bind(Action::PadB, {In(Device::Key, 6)}));

  Profile p = base;
  Overlay(p, user);
  REQUIRE(p.chordWindowMs == 40);
  REQUIRE(p.bindings.size() == 5);
  REQUIRE(p.bindings[0] == base.bindings[0]); // keyboard Fire kept
  REQUIRE(p.bindings[1] == base.bindings[3]); // touch Z kept
  REQUIRE(p.bindings[2] == base.bindings[4]); // pad B kept
  REQUIRE(p.bindings[3] == user.bindings[0]);
  REQUIRE(p.bindings[4] == user.bindings[1]);

  user.replace = true;
  user.chordWindowMs = 90;
  p = base;
  Overlay(p, user);
  REQUIRE(p.chordWindowMs == 90);
  REQUIRE(p.bindings == user.bindings);
}

void TestFamilyEditing() {
  Profile inherited;
  inherited.bindings.push_back(Bind(Action::PadA, {In(Device::Key, 44)}));
  inherited.bindings.push_back(Bind(Action::PadA, {In(Device::Key, 45)}));
  inherited.bindings.push_back(Bind(Action::PadA, {In(Device::PadButton, 0)}));
  inherited.bindings.push_back(Bind(Action::PadB, {In(Device::PadButton, 1)}));

  UserProfile user;
  REQUIRE(BindingFamily(inherited.bindings[2]) == Family::Pad);
  REQUIRE(!Overridden(user, Action::PadA, Family::Keyboard));

  // Editing one of two inherited keys keeps the other, and the pad binding alone.
  Materialize(user, Action::PadA, Family::Keyboard, inherited);
  REQUIRE(user.bindings.size() == 2);
  REQUIRE(Overridden(user, Action::PadA, Family::Keyboard));
  REQUIRE(!Overridden(user, Action::PadA, Family::Pad));
  Materialize(user, Action::PadA, Family::Keyboard, inherited); // already overridden: no copy
  REQUIRE(user.bindings.size() == 2);
  Profile p = inherited;
  Overlay(p, user);
  REQUIRE(p.bindings.size() == 4);
  REQUIRE(InFamily(p.bindings, Action::PadA, Family::Pad).size() == 1);

  // Removing the last one unbinds the family.
  RemoveUserBinding(user, 0);
  REQUIRE(user.unbind.empty());
  RemoveUserBinding(user, 0);
  REQUIRE(user.bindings.empty());
  REQUIRE(user.unbind.size() == 1 && user.unbind[0] == (Unbind{Action::PadA, Family::Keyboard}));
  p = inherited;
  Overlay(p, user);
  REQUIRE(InFamily(p.bindings, Action::PadA, Family::Keyboard).empty());
  REQUIRE(InFamily(p.bindings, Action::PadA, Family::Pad).size() == 1);

  // A new binding supersedes the unbind.
  AddUserBinding(user, Bind(Action::PadA, {In(Device::Key, 9)}));
  REQUIRE(user.unbind.empty() && user.bindings.size() == 1);

  // Revert and ClearFamily.
  AddUserBinding(user, Bind(Action::PadB, {In(Device::PadButton, 2)}));
  Revert(user, Action::PadA, Family::Keyboard);
  REQUIRE(!Overridden(user, Action::PadA, Family::Keyboard));
  REQUIRE(Overridden(user, Action::PadB, Family::Pad));
  user.unbind.push_back({Action::PadZ, Family::Keyboard});
  ClearFamily(user, Family::Pad);
  REQUIRE(user.bindings.empty());
  REQUIRE(user.unbind.size() == 1);
  ClearFamily(user, Family::Keyboard);
  REQUIRE(user.unbind.empty());
}
void TestDuplicateBindings() {
  const Binding key = Bind(Action::PadA, {In(Device::Key, 44)});
  Profile inherited;
  inherited.bindings.push_back(key);
  UserProfile user;
  Materialize(user, key.action, Family::Keyboard, inherited);
  for (int i = 0; i < 5; ++i) AddUserBinding(user, key);
  REQUIRE(user.bindings.size() == 1);

  Binding other = key;
  other.action = Action::PadB;
  AddUserBinding(user, other);
  other = key;
  other.trigger = Trigger::Tap;
  AddUserBinding(user, other);
  REQUIRE(user.bindings.size() == 3);

  // Editing another key to the existing key coalesces the duplicate.
  AddUserBinding(user, Bind(Action::PadA, {In(Device::Key, 45)}));
  RemoveUserBinding(user, 3);
  AddUserBinding(user, key);
  REQUIRE(user.bindings.size() == 3);
  REQUIRE(user.unbind.empty());

  other = key;
  other.contexts = kCtxMorphBall;
  AddUserBinding(user, other);
  AddUserBinding(user, other);
  REQUIRE(user.bindings.size() == 4);

  UserProfile controllerProfile;
  AddUserBinding(controllerProfile, key);
  REQUIRE(controllerProfile.bindings.size() == 1);
}
} // namespace

int main() {
  TestDuplicateBindings();
  TestFamilyEditing();
  TestInputText();
  TestContexts();
  TestRoundTrip();
  TestErrors();
  TestSelect();
  TestOverlay();
  std::puts("input bindings ok");
  return 0;
}

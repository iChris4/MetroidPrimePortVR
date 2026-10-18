#include "port_discord.h"
#include "port_json.h"
#include "port_test.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace {

bool ParseOk(const std::string& text, PortJson::Value& out) {
  size_t offset = 0;
  const char* reason = nullptr;
  return PortJson::Parse(text, out, offset, &reason);
}
} // namespace

int main() {
  using namespace PortDiscord;

  // Frames: little-endian opcode and length, then the payload.
  {
    const std::string frame = EncodeFrame(kOp_Frame, "{}");
    REQUIRE(frame == std::string("\x01\x00\x00\x00\x02\x00\x00\x00{}", 10));
    std::string buffer = frame.substr(0, 9);
    Frame out;
    bool bad = true;
    REQUIRE(!TakeFrame(buffer, out, bad) && !bad);
    buffer += frame.substr(9) + EncodeFrame(kOp_Ping, "p");
    REQUIRE(TakeFrame(buffer, out, bad) && out.op == kOp_Frame && out.payload == "{}");
    REQUIRE(TakeFrame(buffer, out, bad) && out.op == kOp_Ping && out.payload == "p");
    REQUIRE(buffer.empty() && !TakeFrame(buffer, out, bad) && !bad);
    std::string big = EncodeFrame(kOp_Frame, std::string(300, 'x'));
    REQUIRE(static_cast<unsigned char>(big[5]) == 1 && static_cast<unsigned char>(big[4]) == 44);
    big[6] = 1; // 65836 bytes: over the limit
    REQUIRE(!TakeFrame(big, out, bad) && bad);
  }

  // JSON strings and field limits.
  {
    REQUIRE(port::JsonQuote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
    REQUIRE(port::JsonQuote("Ph\xc3\xa9ndrana") == "\"Ph\xc3\xa9ndrana\"");
    REQUIRE(FitField("x") == "x ");
    REQUIRE(FitField("xy") == "xy");
    std::string longText(127, 'a');
    longText += "\xc3\xa9\xc3\xa9"; // the cut falls inside the first é
    REQUIRE(FitField(longText) == std::string(127, 'a'));
    REQUIRE(FitField(std::string(200, 'b')).size() == 128);
  }

  // Game text: tags stripped, && kept, UTF-16 to UTF-8, surrogates.
  {
    const char16_t text[] = u"&just=center;Phéndrana&&Drifts\n2";
    REQUIRE(GameTextToUtf8(text) == "Ph\xc3\xa9ndrana&Drifts 2");
    const char16_t pair[] = {0xd83d, 0xde00, 'a', 0xdc00, 'b', 0};
    REQUIRE(GameTextToUtf8(pair) == "\xf0\x9f\x98\x80" "ab");
    const char16_t open[] = u"A&unterminated";
    REQUIRE(GameTextToUtf8(open) == "A");
    REQUIRE(GameTextToUtf8(static_cast<const wchar_t*>(nullptr)).empty());
    const wchar_t wide[] = L"Landing Site";
    REQUIRE(GameTextToUtf8(wide) == "Landing Site");
  }

  // Payloads parse and carry what Discord expects.
  {
    PortJson::Value value;
    REQUIRE(ParseOk(HandshakePayload("123"), value));
    REQUIRE(value.Find("v")->AsInt() == 1 && value.StringOr("client_id") == "123");

    GameInfo info;
    info.world = "Tallon Overworld";
    info.area = "Landing Site";
    info.percent = 12;
    info.hard = true;
    info.energy = 199;
    info.missiles = 25;
    const Presence game = GamePresence(info, 1700000000);
    REQUIRE(game.details == "Landing Site");
    REQUIRE(game.state == "199 energy \xc2\xb7 25 missiles \xc2\xb7 12% items");
    REQUIRE(game.hover == "Tallon Overworld \xc2\xb7 Hard mode");
    REQUIRE(ParseOk(ActivityPayload(42, game, "7"), value));
    REQUIRE(value.StringOr("cmd") == "SET_ACTIVITY" && value.StringOr("nonce") == "7");
    const PortJson::Value* args = value.Find("args");
    REQUIRE(args != nullptr && args->Find("pid")->AsInt() == 42);
    const PortJson::Value* activity = args->Find("activity");
    REQUIRE(activity != nullptr && activity->StringOr("details") == "Landing Site");
    REQUIRE(activity->StringOr("state") == game.state);
    REQUIRE(activity->Find("timestamps")->Find("start")->AsInt() == 1700000000);
    REQUIRE(activity->Find("assets")->StringOr("large_image") == "logo");
    REQUIRE(activity->Find("assets")->StringOr("large_text") == game.hover);
    REQUIRE(game.image.empty() && activity->Find("assets")->Find("small_image") == nullptr);

    // World pictures (always on): the world's asset takes the large image, the logo the
    // small one. A world without one (the end cinema) keeps the logo.
    info.worldId = 0x83F6FF6Fu;
    const Presence pictured = GamePresence(info, 0);
    REQUIRE(pictured.image == "world_chozo");
    REQUIRE(ParseOk(ActivityPayload(42, pictured, "9"), value));
    const PortJson::Value* assets = value.Find("args")->Find("activity")->Find("assets");
    REQUIRE(assets->StringOr("large_image") == "world_chozo");
    REQUIRE(assets->StringOr("small_image") == "logo");
    REQUIRE(pictured != game);
    info.worldId = 0x13D79165u;
    REQUIRE(GamePresence(info, 0).image.empty());
    REQUIRE(std::strcmp(WorldImage(0x158EFE17u), "world_frigate") == 0);
    REQUIRE(std::strcmp(WorldImage(0xC13B09D1u), "world_crater") == 0);

    // Names still loading: the world alone, then a generic line. Without a
    // launcher there's no missile count; without a world, the default tooltip.
    GameInfo early;
    early.world = "Chozo Ruins";
    early.energy = 99;
    early.missiles = 1;
    REQUIRE(GamePresence(early, 0).details == "Chozo Ruins");
    REQUIRE(GamePresence(early, 0).state == "99 energy \xc2\xb7 1 missile \xc2\xb7 0% items");
    REQUIRE(GamePresence(early, 0).hover.empty());
    early.world.clear();
    early.missiles = -1;
    REQUIRE(GamePresence(early, 0).details == "In game");
    REQUIRE(GamePresence(early, 0).state == "99 energy \xc2\xb7 0% items");
    REQUIRE(ParseOk(ActivityPayload(1, GamePresence(early, 0), "8"), value));
    REQUIRE(value.Find("args")->Find("activity")->Find("assets")->StringOr("large_text") ==
            "Metroid Prime native port");

    Presence menu;
    menu.details = "In the menus";
    REQUIRE(ParseOk(ActivityPayload(1, menu, "1"), value));
    activity = value.Find("args")->Find("activity");
    REQUIRE(activity->Find("state") == nullptr && activity->Find("timestamps") == nullptr);

    REQUIRE(ParseOk(ClearPayload(9, "2"), value));
    REQUIRE(value.Find("args")->Find("activity") == nullptr);
  }

  // Golden payloads, byte for byte as the hand-assembled writer made them
  // before port::JsonWriter replaced it.
  {
    REQUIRE(HandshakePayload("12\"34") == R"({"v":1,"client_id":"12\"34"})");
    Presence p;
    p.details = "Tallon \"Overworld\"";
    REQUIRE(ActivityPayload(42, p, "n1") ==
            R"({"cmd":"SET_ACTIVITY","args":{"pid":42,"activity":{"details":"Tallon \"Overworld\"",)"
            R"("assets":{"large_image":"logo","large_text":"Metroid Prime native port"}}},"nonce":"n1"})");
    p.state = "x";
    p.start = 1790000000;
    p.image = "world_tallon";
    p.hover = "Landing Site\n";
    REQUIRE(ActivityPayload(-1, p, "n\\2") ==
            R"({"cmd":"SET_ACTIVITY","args":{"pid":-1,"activity":{"details":"Tallon \"Overworld\"",)"
            R"("state":"x ","timestamps":{"start":1790000000},"assets":{"large_image":"world_tallon",)"
            R"("large_text":"Landing Site\n","small_image":"logo","small_text":"Metroid Prime native port"}}},)"
            R"("nonce":"n\\2"})");
    p.hover.clear();
    REQUIRE(ActivityPayload(7, p, "") ==
            R"({"cmd":"SET_ACTIVITY","args":{"pid":7,"activity":{"details":"Tallon \"Overworld\"",)"
            R"("state":"x ","timestamps":{"start":1790000000},"assets":{"large_image":"world_tallon",)"
            R"("large_text":"Metroid Prime native port","small_image":"logo",)"
            R"("small_text":"Metroid Prime native port"}}},"nonce":""})");
    REQUIRE(ClearPayload(9, "n3") == R"({"cmd":"SET_ACTIVITY","args":{"pid":9},"nonce":"n3"})");
  }

  // Socket search order.
#if !defined(_WIN32)
  {
    std::map<std::string, std::string> env = {{"TMPDIR", "/var/tmp/"}};
    auto get = [&](const char* name) -> const char* {
      auto it = env.find(name);
      return it == env.end() ? nullptr : it->second.c_str();
    };
    auto paths = SocketCandidates(get);
    REQUIRE(paths.size() == 30 && paths[0] == "/var/tmp/discord-ipc-0");
    REQUIRE(paths[10] == "/var/tmp/app/com.discordapp.Discord/discord-ipc-0");
    REQUIRE(paths[29] == "/var/tmp/snap.discord/discord-ipc-9");
    env["XDG_RUNTIME_DIR"] = "/run/user/1000";
    REQUIRE(SocketCandidates(get)[0] == "/run/user/1000/discord-ipc-0");
    env.clear();
    REQUIRE(SocketCandidates(get)[0] == "/tmp/discord-ipc-0");
  }
#endif

  std::puts("discord tests passed");
  return 0;
}

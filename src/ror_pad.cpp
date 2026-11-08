// Pad seam: host keyboard and gamepad (raylib, the API the runtime window and its own ps2_pad.cpp use) mapped to
// the raw libpad2 data bedpad 0x101ccc reads through 0x26d320: u16 buttons at +0, active-low, stick bytes +2..+5
// centred at 0x80. The mapping is config/pad.ini (ROR_PAD_CONFIG). ROR_PAD_SCRIPT presses buttons at given frames
// for unattended runs: "FRAME+N:button,button;..." holds the buttons for N frames from FRAME; an item "rx=HH",
// "ry=HH", "lx=HH" or "ly=HH" holds that stick byte (+2..+5, pad.ini's order) at hex HH instead; "go=X/Z" steers the
// left stick each frame towards the world point (X, Z) (running; walking within 60 units; released within 12);
// "seek=X/Z" the same, stepping aside around obstacles; "path=X/Z>X/Z>..." seeks each point in turn, the next once
// within 24 units (so a delay does not cut the walk short), and ends at the last point or on an area change;
// "autoshake" (latched from its step's first frame) shakes the left stick whenever an enemy holds her.
// "go=@ID" steers towards a character object (hex id, e.g. an enemy 9100004b) wherever it is each frame.
// "fastshake" (latched) does it on every frame, freeing her in about 16 frames (see fight()).
// "autofight" (latched from its step's first frame) fights any enemy with health left that comes within range,
// overriding the stick, R1 and Cross while it does (fight() below).
#include "ror.h"

#include "ps2_host_backend.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    const std::map<std::string, int> kButtons = {
        {"select", 0}, {"l3", 1}, {"r3", 2}, {"start", 3}, {"up", 4}, {"right", 5}, {"down", 6}, {"left", 7},
        {"l2", 8}, {"r2", 9}, {"l1", 10}, {"r1", 11}, {"triangle", 12}, {"circle", 13}, {"cross", 14}, {"square", 15}};

    const std::map<std::string, int> kKeys = {
        {"A", KEY_A}, {"B", KEY_B}, {"C", KEY_C}, {"D", KEY_D}, {"E", KEY_E}, {"F", KEY_F}, {"G", KEY_G},
        {"H", KEY_H}, {"I", KEY_I}, {"J", KEY_J}, {"K", KEY_K}, {"L", KEY_L}, {"M", KEY_M}, {"N", KEY_N},
        {"O", KEY_O}, {"P", KEY_P}, {"Q", KEY_Q}, {"R", KEY_R}, {"S", KEY_S}, {"T", KEY_T}, {"U", KEY_U},
        {"V", KEY_V}, {"W", KEY_W}, {"X", KEY_X}, {"Y", KEY_Y}, {"Z", KEY_Z},
        {"ZERO", KEY_ZERO}, {"ONE", KEY_ONE}, {"TWO", KEY_TWO}, {"THREE", KEY_THREE}, {"FOUR", KEY_FOUR},
        {"FIVE", KEY_FIVE}, {"SIX", KEY_SIX}, {"SEVEN", KEY_SEVEN}, {"EIGHT", KEY_EIGHT}, {"NINE", KEY_NINE},
        {"SPACE", KEY_SPACE}, {"ENTER", KEY_ENTER}, {"TAB", KEY_TAB}, {"ESCAPE", KEY_ESCAPE},
        {"BACKSPACE", KEY_BACKSPACE}, {"UP", KEY_UP}, {"DOWN", KEY_DOWN}, {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT},
        {"LEFT_SHIFT", KEY_LEFT_SHIFT}, {"RIGHT_SHIFT", KEY_RIGHT_SHIFT}, {"LEFT_CONTROL", KEY_LEFT_CONTROL},
        {"RIGHT_CONTROL", KEY_RIGHT_CONTROL}, {"LEFT_ALT", KEY_LEFT_ALT}, {"RIGHT_ALT", KEY_RIGHT_ALT},
        {"INSERT", KEY_INSERT}, {"DELETE", KEY_DELETE}, {"HOME", KEY_HOME}, {"END", KEY_END},
        {"PAGE_UP", KEY_PAGE_UP}, {"PAGE_DOWN", KEY_PAGE_DOWN}, {"COMMA", KEY_COMMA}, {"PERIOD", KEY_PERIOD},
        {"SLASH", KEY_SLASH}, {"SEMICOLON", KEY_SEMICOLON}, {"MINUS", KEY_MINUS}, {"EQUAL", KEY_EQUAL},
        {"F1", KEY_F1}, {"F2", KEY_F2}, {"F3", KEY_F3}, {"F4", KEY_F4}, {"F5", KEY_F5}, {"F6", KEY_F6},
        {"F7", KEY_F7}, {"F8", KEY_F8}, {"F9", KEY_F9}, {"F10", KEY_F10}, {"F11", KEY_F11}, {"F12", KEY_F12},
        {"KP_0", KEY_KP_0}, {"KP_1", KEY_KP_1}, {"KP_2", KEY_KP_2}, {"KP_3", KEY_KP_3}, {"KP_4", KEY_KP_4},
        {"KP_5", KEY_KP_5}, {"KP_6", KEY_KP_6}, {"KP_7", KEY_KP_7}, {"KP_8", KEY_KP_8}, {"KP_9", KEY_KP_9},
        {"KP_ENTER", KEY_KP_ENTER}};

    const std::map<std::string, int> kPadButtons = {
        {"LEFT_FACE_UP", GAMEPAD_BUTTON_LEFT_FACE_UP}, {"LEFT_FACE_RIGHT", GAMEPAD_BUTTON_LEFT_FACE_RIGHT},
        {"LEFT_FACE_DOWN", GAMEPAD_BUTTON_LEFT_FACE_DOWN}, {"LEFT_FACE_LEFT", GAMEPAD_BUTTON_LEFT_FACE_LEFT},
        {"RIGHT_FACE_UP", GAMEPAD_BUTTON_RIGHT_FACE_UP}, {"RIGHT_FACE_RIGHT", GAMEPAD_BUTTON_RIGHT_FACE_RIGHT},
        {"RIGHT_FACE_DOWN", GAMEPAD_BUTTON_RIGHT_FACE_DOWN}, {"RIGHT_FACE_LEFT", GAMEPAD_BUTTON_RIGHT_FACE_LEFT},
        {"LEFT_TRIGGER_1", GAMEPAD_BUTTON_LEFT_TRIGGER_1}, {"LEFT_TRIGGER_2", GAMEPAD_BUTTON_LEFT_TRIGGER_2},
        {"RIGHT_TRIGGER_1", GAMEPAD_BUTTON_RIGHT_TRIGGER_1}, {"RIGHT_TRIGGER_2", GAMEPAD_BUTTON_RIGHT_TRIGGER_2},
        {"MIDDLE_LEFT", GAMEPAD_BUTTON_MIDDLE_LEFT}, {"MIDDLE", GAMEPAD_BUTTON_MIDDLE},
        {"MIDDLE_RIGHT", GAMEPAD_BUTTON_MIDDLE_RIGHT}, {"LEFT_THUMB", GAMEPAD_BUTTON_LEFT_THUMB},
        {"RIGHT_THUMB", GAMEPAD_BUTTON_RIGHT_THUMB}};

    const std::map<std::string, int> kAxes = {
        {"LEFT_X", GAMEPAD_AXIS_LEFT_X}, {"LEFT_Y", GAMEPAD_AXIS_LEFT_Y},
        {"RIGHT_X", GAMEPAD_AXIS_RIGHT_X}, {"RIGHT_Y", GAMEPAD_AXIS_RIGHT_Y}};

    struct Mapping
    {
        std::vector<int> keys[16], pads[16];
        std::vector<int> axes[4], keyMinus[4], keyPlus[4];
        std::vector<int> ffKeys, ffPads;   // fast_forward: host action, not a PS2 button
    };

    // Obstacles: when the player has not moved for 8 frames, steer 80 degrees to one side for 25 frames; when stuck
    // again during that detour, the other side, for longer. Reset when she moves freely towards the target again.
    struct Detour
    {
        float lastX = 0, lastZ = 0;
        int still = 0, until = 0, side = 1, frames = 0, tries = 0;
    };

    struct ScriptStep
    {
        uint64_t from, to;
        uint16_t buttons; // set bits = pressed
        int sticks[4];    // stick bytes +2..+5, -1 = not set by this step
        int go;           // steer to (goX, goZ): 1 straight, 2 around obstacles
        float goX, goZ;
        bool autoshake;   // from this step's first frame to the end of the run: shake free of grabs
        bool autofight;   // likewise: fight enemies that come near (fight())
        bool fastshake;   // likewise: shake free on every frame
        uint32_t target;  // "go=@ID": the character object to steer to (0 = the fixed goX, goZ)
        std::vector<std::pair<float, float>> path; // "path=": waypoints, each passed when within kPathReach
        size_t next = 0;
        float lastX = 0, lastZ = 0;
        Detour detour;    // a path's own: a detour still running from an earlier step (one that ended stuck, or
                          // steered on through an event) turned the next path's first frames 80 degrees off its target
    };

    constexpr float kPathReach = 24.0f;

    // Steering for "go=X/Z": the player's position (GamMan+0x22020, GamMan::update 0x1c6580) and the camera 0x750810
    // (MagScene+0x2acd0), whose yaw is atan2 of its +0x20/+0x28 (0x10fcbc). The player's heading is the stick's
    // atan2(x, -y) plus that yaw (0x11b2d8) and a heading h moves along (sin h, cos h) in x/z.
    constexpr uint32_t kPlayerPos = 0x7cd1e0, kCamera = 0x750810;

    // The player's mode, 0x713600+0x3d460 (FUN_0011919c, which the trigger regions' Cross test 0x1e0204 asks): 1 idle,
    // 3 moving, 8 held by an enemy (observed: chapter 2's imps, `ch2_gift` frames 1520 and 2180), then 0xe/0xc/0xd
    // while she falls and gets up. Shaking the left stick sideways frees her (the A003 tutorial).
    constexpr uint32_t kPlayerMode = 0x750a60;
    constexpr uint32_t kModeHeld = 8;

    float rdf(uint8_t *rdram, uint32_t addr)
    {
        const uint32_t u = ror::rd32(rdram, addr);
        float f;
        std::memcpy(&f, &u, sizeof f);
        return f;
    }

    // A stick value -1..1 as the byte bedpad 0x101f74/0x101ffc reads back (dead zone 0x60..0xa0).
    int stickValueByte(float v)
    {
        if (v < -0.01f)
            return std::max(0, static_cast<int>(std::lround(96.0f + 96.0f * v)));
        if (v > 0.01f)
            return std::min(255, static_cast<int>(std::lround(160.0f + 95.0f * v)));
        return 0x80;
    }

    // Characters (object ids with (id >> 28) & 3 == 1): GamMan+0x21340 = 0x7cc500 holds one pointer per id & 0x1ff
    // below 0x80 (0x1d3e64, 0x1d3d84); the character's +0x108 is its record slot (0x1cd0c4), the record's +0x40 its
    // position (the region walk's object test 0x1e068c).
    bool characterPos(uint8_t *rdram, uint32_t id, float &x, float &z)
    {
        if (((id >> 28) & 3) != 1 || (id & 0x1ff) >= 0x80)
            return false;
        const uint32_t obj = ror::rd32(rdram, 0x7cc500 + (id & 0x1ff) * 4);
        if (obj < 0x100000 || obj >= 0x2000000)
            return false;
        const int32_t slot = static_cast<int32_t>(ror::rd32(rdram, obj + 0x108));
        if (slot < 0 || slot >= 0x200)
            return false;
        const uint32_t rec = 0x7285c0 + static_cast<uint32_t>(slot) * 0x4d0;
        x = rdf(rdram, rec + 0x40);
        z = rdf(rdram, rec + 0x48);
        return true;
    }

    // Health: the character's u16 at +0x126 (Jennifer 3000 at full; A035's enemies 640..808; 0 when beaten), seen
    // falling as she is hit and as the enemies are (peeks during the A035 fight).
    int characterHealth(uint8_t *rdram, uint32_t id)
    {
        if (((id >> 28) & 3) != 1 || (id & 0x1ff) >= 0x80)
            return 0;
        const uint32_t obj = ror::rd32(rdram, 0x7cc500 + (id & 0x1ff) * 4);
        if (obj < 0x100000 || obj >= 0x2000000)
            return 0;
        return ror::rd32(rdram, obj + 0x124) >> 16;
    }

    Detour g_detour;   // shared by the "go=" steps (the recorded routes replay with it); each "path=" step has its own

    void steer(uint8_t *rdram, float x, float z, bool around, Detour &d, int sticks[4])
    {
        const float px = rdf(rdram, kPlayerPos), pz = rdf(rdram, kPlayerPos + 8);
        const float dx = x - px, dz = z - pz;
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (dist < 12.0f)
        {
            sticks[2] = sticks[3] = 0x80;
            return;
        }
        ++d.frames;
        const float moved = std::abs(px - d.lastX) + std::abs(pz - d.lastZ);
        d.lastX = px;
        d.lastZ = pz;
        d.still = moved < 0.5f ? d.still + 1 : 0;
        if (d.still >= 8)
        {
            d.still = 0;
            if (d.frames < d.until)
                d.side = -d.side;
            d.until = d.frames + 25 + 15 * std::min(d.tries, 6);
            ++d.tries;
        }
        else if (d.frames >= d.until && moved > 2.0f)
            d.tries = 0;
        const float detour = around && d.frames < d.until ? d.side * 1.396f : 0.0f;  // 80 degrees
        const float a = std::atan2(dx, dz) + detour - std::atan2(rdf(rdram, kCamera + 0x20), rdf(rdram, kCamera + 0x28));
        const float mag = dist < 60.0f ? 0.6f : 1.0f;
        sticks[2] = stickValueByte(std::sin(a) * mag);
        sticks[3] = stickValueByte(-std::cos(a) * mag);
    }

    // "autofight": the enemies are the enemy table's (0x7dc040: 64 record pointers from +4, record +0 the object id;
    // n212's lookup 0x163ca4). One within kFightRange (|dy| < 150) with health left is fought in rounds of
    // kFightRound frames, the nearest picked at each round's start: kFightTurn frames turning towards it (R1
    // released), then R1 held (the attack stance, 0x11aea4) and Cross every 15 frames (an attack: Cross in the stance
    // modes starts mode 10, 0x11b4bc). The A035 fight (three enemies, 640..808 health, the Dessert Fork) is won this
    // way, where steering while R1 is held does not turn her. Nothing happens while the player mode is 0 (events).
    constexpr float kFightRange = 300.0f;
    constexpr uint64_t kFightRound = 160, kFightTurn = 20;
    struct Fight
    {
        uint32_t target = 0;
        uint64_t start = 0;
    } g_fight;

    // true when it drives the pad this frame
    bool fight(uint8_t *rdram, uint64_t f, uint16_t &pressed, int sticks[4])
    {
        const uint32_t mode = ror::rd32(rdram, kPlayerMode);
        if (mode == 0)
            return false;
        // Held: MagLifePCModeHold::pad 0x1229f0 lets go at 16 on the pad's shake counter (+0x64), which bedpad's
        // 0x102188 raises each time the stick's direction changes within 15 frames of the last change: flipping
        // every frame frees her in about 16 frames (autoshake's every 4th frame, kept for the recorded routes, 64).
        if (mode == kModeHeld)
        {
            sticks[2] = f & 1 ? 0xff : 0x00;
            sticks[3] = 0x80;
            pressed &= static_cast<uint16_t>(~((1u << 11) | (1u << 14)));
            return true;
        }
        const float px = rdf(rdram, kPlayerPos), py = rdf(rdram, kPlayerPos + 4), pz = rdf(rdram, kPlayerPos + 8);
        float ex = 0, ez = 0;
        if (!g_fight.target || f - g_fight.start >= kFightRound || characterHealth(rdram, g_fight.target) <= 0)
        {
            g_fight.target = 0;
            float best = kFightRange;
            for (uint32_t i = 0; i < 64; ++i)
            {
                const uint32_t rec = ror::rd32(rdram, 0x7dc044 + i * 4);
                if (rec < 0x100000 || rec >= 0x2000000)
                    continue;
                const uint32_t id = ror::rd32(rdram, rec);
                float x, z;
                if (characterHealth(rdram, id) <= 0 || !characterPos(rdram, id, x, z))
                    continue;
                const uint32_t obj = ror::rd32(rdram, 0x7cc500 + (id & 0x1ff) * 4);
                const int32_t slot = static_cast<int32_t>(ror::rd32(rdram, obj + 0x108));
                const float y = rdf(rdram, 0x7285c0 + static_cast<uint32_t>(slot) * 0x4d0 + 0x44);
                const float d = std::hypot(x - px, z - pz);
                if (d < best && std::abs(y - py) < 150.0f)
                {
                    best = d;
                    g_fight.target = id;
                }
            }
            g_fight.start = f;
            if (g_fight.target)
                ror::log("pad: autofight: %08x (health %d), Jennifer %d", g_fight.target,
                         characterHealth(rdram, g_fight.target), characterHealth(rdram, 0x50000001));
        }
        if (!g_fight.target || !characterPos(rdram, g_fight.target, ex, ez))
            return false;
        const uint64_t t = f - g_fight.start;
        pressed &= static_cast<uint16_t>(~((1u << 11) | (1u << 14)));
        if (t < kFightTurn)
        {
            Detour none;
            steer(rdram, ex, ez, false, none, sticks);
        }
        else
        {
            sticks[2] = sticks[3] = 0x80;
            pressed |= 1u << 11;
            if (t >= kFightTurn + 5 && (t - kFightTurn - 5) % 15 < 3)
                pressed |= 1u << 14;
        }
        return true;
    }

    const std::map<std::string, int> kScriptSticks = {{"rx", 0}, {"ry", 1}, {"lx", 2}, {"ly", 3}};

    std::once_flag g_loaded;
    Mapping g_map;
    std::vector<ScriptStep> g_script;

    void loadConfig()
    {
        const char *env = std::getenv("ROR_PAD_CONFIG");
        const std::string path = env && *env ? env : "pad.ini";
        std::ifstream in(path);
        if (!in)
        {
            ror::log("pad: no mapping file %s, only ROR_PAD_SCRIPT input", path.c_str());
            return;
        }
        std::string line;
        int lineNo = 0, inputs = 0;
        while (std::getline(in, line))
        {
            ++lineNo;
            const size_t hash = line.find('#');
            if (hash != std::string::npos)
                line.resize(hash);
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string name = line.substr(0, eq);
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            std::istringstream items(line.substr(eq + 1));
            const auto button = kButtons.find(name);
            const int stick = (name.size() == 6 && name.compare(0, 5, "stick") == 0) ? name[5] - '2' : -1;
            const bool ff = name == "fast_forward";
            if (button == kButtons.end() && (stick < 0 || stick > 3) && !ff)
            {
                ror::log("pad: %s:%d: unknown name %s", path.c_str(), lineNo, name.c_str());
                continue;
            }
            for (std::string item; items >> item;)
            {
                const size_t colon = item.find(':');
                const std::string kind = item.substr(0, colon), what = colon == std::string::npos ? "" : item.substr(colon + 1);
                auto add = [&](const std::map<std::string, int> &table, std::vector<int> &into)
                {
                    const auto it = table.find(what);
                    if (it == table.end())
                        ror::log("pad: %s:%d: unknown %s %s", path.c_str(), lineNo, kind.c_str(), what.c_str());
                    else
                        into.push_back(it->second), ++inputs;
                };
                if (ff && kind == "key")
                    add(kKeys, g_map.ffKeys);
                else if (ff && kind == "pad")
                    add(kPadButtons, g_map.ffPads);
                else if (button != kButtons.end() && kind == "key")
                    add(kKeys, g_map.keys[button->second]);
                else if (button != kButtons.end() && kind == "pad")
                    add(kPadButtons, g_map.pads[button->second]);
                else if (stick >= 0 && kind == "axis")
                    add(kAxes, g_map.axes[stick]);
                else if (stick >= 0 && kind == "key-")
                    add(kKeys, g_map.keyMinus[stick]);
                else if (stick >= 0 && kind == "key+")
                    add(kKeys, g_map.keyPlus[stick]);
                else
                    ror::log("pad: %s:%d: %s does not apply to %s", path.c_str(), lineNo, item.c_str(), name.c_str());
            }
        }
        ror::log("pad: %d inputs mapped from %s", inputs, path.c_str());
    }

    void loadScript()
    {
        const char *env = std::getenv("ROR_PAD_SCRIPT");
        if (!env || !*env)
            return;
        std::istringstream steps(env);
        for (std::string step; std::getline(steps, step, ';');)
        {
            unsigned long long from = 0, count = 1;
            char names[256] = {};
            if (std::sscanf(step.c_str(), "%llu+%llu:%255s", &from, &count, names) != 3)
            {
                ror::log("pad: bad ROR_PAD_SCRIPT step '%s'", step.c_str());
                continue;
            }
            uint16_t buttons = 0;
            int sticks[4] = {-1, -1, -1, -1};
            int go = 0;
            float goX = 0, goZ = 0;
            bool autoshake = false, autofight = false, fastshake = false;
            uint32_t target = 0;
            std::vector<std::pair<float, float>> path;
            std::istringstream list(names);
            for (std::string b; std::getline(list, b, ',');)
            {
                const auto it = kButtons.find(b);
                if (b == "autoshake")
                {
                    autoshake = true;
                    continue;
                }
                if (b == "autofight")
                {
                    autofight = true;
                    continue;
                }
                if (b == "fastshake")
                {
                    fastshake = true;
                    continue;
                }
                if (b.compare(0, 5, "path=") == 0)
                {
                    std::istringstream points(b.substr(5));
                    for (std::string pt; std::getline(points, pt, '>');)
                        if (std::sscanf(pt.c_str(), "%f/%f", &goX, &goZ) == 2)
                            path.emplace_back(goX, goZ);
                    go = path.empty() ? 0 : 2;
                    continue;
                }
                if (b.compare(0, 4, "go=@") == 0)
                {
                    target = static_cast<uint32_t>(std::strtoul(b.c_str() + 4, nullptr, 16));
                    go = 1;
                    continue;
                }
                if (b.compare(0, 3, "go=") == 0 && std::sscanf(b.c_str() + 3, "%f/%f", &goX, &goZ) == 2)
                {
                    go = 1;
                    continue;
                }
                if (b.compare(0, 5, "seek=") == 0 && std::sscanf(b.c_str() + 5, "%f/%f", &goX, &goZ) == 2)
                {
                    go = 2;
                    continue;
                }
                const auto stick = b.size() > 3 && b[2] == '=' ? kScriptSticks.find(b.substr(0, 2)) : kScriptSticks.end();
                if (stick != kScriptSticks.end())
                    sticks[stick->second] = static_cast<int>(std::strtoul(b.c_str() + 3, nullptr, 16) & 0xff);
                else if (it == kButtons.end())
                    ror::log("pad: unknown button '%s' in ROR_PAD_SCRIPT", b.c_str());
                else
                    buttons |= static_cast<uint16_t>(1u << it->second);
            }
            g_script.push_back({from, from + count, buttons, {sticks[0], sticks[1], sticks[2], sticks[3]}, go, goX, goZ,
                                autoshake, autofight, fastshake, target, path});
            ror::log("pad: script presses %s on frames %llu-%llu", names, from, from + count - 1);
        }
    }

    uint8_t stickByte(int i)
    {
        float v = 0.0f;
        for (int axis : g_map.axes[i])
        {
            const float a = GetGamepadAxisMovement(0, axis);
            if (a * a > v * v)
                v = a;
        }
        for (int key : g_map.keyMinus[i])
            if (IsKeyDown(key))
                v = -1.0f;
        for (int key : g_map.keyPlus[i])
            if (IsKeyDown(key))
                v = 1.0f;
        const int byte = 0x80 + static_cast<int>(v * 127.0f);
        return static_cast<uint8_t>(byte < 0 ? 0 : byte > 255 ? 255 : byte);
    }
}

// scePad2CreateSocket(param, buffer): bedpad 0x101bcc puts the port at param+4; the socket is the port.
void ror_scePad2CreateSocket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    ror::ret(ctx, ror::rd32(rdram, ror::arg(ctx, 0) + 4));
}

// 0x26d320(socket, data): copy the raw report. Returns the data length (bedpad ignores it).
void ror_pad_read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    std::call_once(g_loaded, []
                   { loadConfig(); loadScript(); });
    const uint32_t socket = ror::arg(ctx, 0), data = ror::arg(ctx, 1);
    uint16_t pressed = 0;
    int scripted[4] = {-1, -1, -1, -1};
    if (socket == 0) // port 1 only; port 2 reads as connected with nothing pressed
    {
        const bool pad = IsGamepadAvailable(0);
        for (int b = 0; b < 16; ++b)
        {
            bool down = false;
            for (int key : g_map.keys[b])
                down |= IsKeyDown(key);
            if (pad)
                for (int button : g_map.pads[b])
                    down |= IsGamepadButtonDown(0, button);
            if (down)
                pressed |= static_cast<uint16_t>(1u << b);
        }
        const uint64_t f = ror::frame();
        bool shake = false, fights = false, fast = false;
        for (ScriptStep &s : g_script)
        {
            shake |= s.autoshake && f >= s.from;
            fights |= s.autofight && f >= s.from;
            fast |= s.fastshake && f >= s.from;
            if (f >= s.from && f < s.to)
            {
                pressed |= s.buttons;
                if (!s.path.empty())
                {
                    // Done at the last point, or when she is moved far in one frame (a new area): the step ends there.
                    const float px = rdf(rdram, kPlayerPos), pz = rdf(rdram, kPlayerPos + 8);
                    while (s.next + 1 < s.path.size() &&
                           std::hypot(s.path[s.next].first - px, s.path[s.next].second - pz) < kPathReach)
                        ++s.next;
                    const bool arrived = s.next + 1 == s.path.size() &&
                                         std::hypot(s.path[s.next].first - px, s.path[s.next].second - pz) < kPathReach;
                    const bool moved = f > s.from && std::hypot(px - s.lastX, pz - s.lastZ) > 300.0f;
                    s.lastX = px;
                    s.lastZ = pz;
                    if (arrived || moved)
                    {
                        ror::log("pad: path %s at (%.0f, %.0f)", arrived ? "done" : "left (area change)", px, pz);
                        s.to = f;
                        continue;
                    }
                    s.goX = s.path[s.next].first;
                    s.goZ = s.path[s.next].second;
                }
                if (s.target && !characterPos(rdram, s.target, s.goX, s.goZ))
                    continue;
                if (s.go)
                    steer(rdram, s.goX, s.goZ, s.go == 2, s.path.empty() ? g_detour : s.detour, scripted);
                for (int i = 0; i < 4; ++i)
                    if (s.sticks[i] >= 0)
                        scripted[i] = s.sticks[i];
            }
        }
        bool fought = fights && fight(rdram, f, pressed, scripted);
        if (fast && !fought && ror::rd32(rdram, kPlayerMode) == kModeHeld)
        {
            scripted[2] = f & 1 ? 0xff : 0x00;
            fought = true;
        }
        if (shake && !fought && ror::rd32(rdram, kPlayerMode) == kModeHeld)
            scripted[2] = (f >> 2) & 1 ? 0xff : 0x00;
    }
    ror::wr16(rdram, data, static_cast<uint16_t>(~pressed));
    for (int i = 0; i < 4; ++i)
        ror::wr8(rdram, data + 2 + i, socket != 0 ? 0x80 : scripted[i] >= 0 ? scripted[i] : stickByte(i));
    ror::ret(ctx, 6);
}

// pad.ini's fast_forward (keyboard F by default): ror_vsync.cpp runs 5 ms VBlanks while it is held. Read on the game
// thread, like the pad itself.
bool ror::fastForwardHeld()
{
    std::call_once(g_loaded, []
                   { loadConfig(); loadScript(); });
    for (int key : g_map.ffKeys)
        if (IsKeyDown(key))
            return true;
    if (IsGamepadAvailable(0))
        for (int button : g_map.ffPads)
            if (IsGamepadButtonDown(0, button))
                return true;
    return false;
}

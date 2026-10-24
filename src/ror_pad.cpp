// Pad seam: host keyboard and gamepad (raylib, the API the runtime window and its own ps2_pad.cpp use) mapped to
// the raw libpad2 data bedpad 0x101ccc reads through 0x26d320: u16 buttons at +0, active-low, stick bytes +2..+5
// centred at 0x80. The mapping is config/pad.ini (ROR_PAD_CONFIG). ROR_PAD_SCRIPT presses buttons at given frames
// for unattended runs: "FRAME+N:button,button;..." holds the buttons for N frames from FRAME.
#include "ror.h"

#include "ps2_host_backend.h"

#include <cctype>
#include <cstdlib>
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
    };

    struct ScriptStep
    {
        uint64_t from, to;
        uint16_t buttons; // set bits = pressed
    };

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
            if (button == kButtons.end() && (stick < 0 || stick > 3))
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
                if (button != kButtons.end() && kind == "key")
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
            std::istringstream list(names);
            for (std::string b; std::getline(list, b, ',');)
            {
                const auto it = kButtons.find(b);
                if (it == kButtons.end())
                    ror::log("pad: unknown button '%s' in ROR_PAD_SCRIPT", b.c_str());
                else
                    buttons |= static_cast<uint16_t>(1u << it->second);
            }
            g_script.push_back({from, from + count, buttons});
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
        for (const ScriptStep &s : g_script)
            if (f >= s.from && f < s.to)
                pressed |= s.buttons;
    }
    ror::wr16(rdram, data, static_cast<uint16_t>(~pressed));
    for (int i = 0; i < 4; ++i)
        ror::wr8(rdram, data + 2 + i, socket == 0 ? stickByte(i) : 0x80);
    ror::ret(ctx, 6);
}

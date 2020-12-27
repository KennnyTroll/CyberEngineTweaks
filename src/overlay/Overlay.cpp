#include "Overlay.h"

#include <atlcomcli.h>
#include <d3d12.h>
#include <Image.h>
#include <imgui.h>
#include <memory>
#include <Options.h>
#include <Pattern.h>
#include <kiero/kiero.h>
#include <spdlog/spdlog.h>
#include <RED4ext/REDhash.hpp>

#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "RED4ext/REDreverse/CString.hpp"
#include "reverse/BasicTypes.h"
#include "reverse/Engine.h"
#include "reverse/Scripting.h"

static std::shared_ptr<Overlay> s_pOverlay;

void Overlay::Initialize(Image* apImage)
{
    if (!s_pOverlay)
    {
        s_pOverlay.reset(new (std::nothrow) Overlay);
        s_pOverlay->EarlyHooks(apImage);
    }
}

void Overlay::release()
{
    s_pOverlay->d3d12Device->Release();
    s_pOverlay->m_pd3dRtvDescHeap->Release();
    s_pOverlay->m_pd3dSrvDescHeap->Release();
    s_pOverlay->m_pd3dCommandList->Release();
    s_pOverlay->m_pCommandQueue->Release();
}

void Overlay::InputHookRemove()
{
    SetWindowLongPtr(s_pOverlay->m_hwnd, GWLP_WNDPROC, (LONG_PTR)s_pOverlay->m_wndProc);
}

void Overlay::Shutdown()
{
    s_pOverlay = nullptr;
}

Overlay& Overlay::Get()
{
    return *s_pOverlay;
}

void Overlay::DrawImgui(IDXGISwapChain3* apSwapChain)
{
    Scripting::Get();

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (IsEnabled())
    {
#pragma region Cyber Engine Tweaks

        ImGui::SetNextWindowSize(ImVec2(600.f, ImGui::GetFrameHeight() * 15.f + ImGui::GetFrameHeightWithSpacing()), ImGuiCond_FirstUseEver);

        ImGui::Begin("Cyber Engine Tweaks");

        auto [major, minor] = Options::Get().GameImage.GetVersion();

        if (major == 1 && (minor >= 4 && minor <= 6))
        {
            ImGui::Checkbox("Clear Input", &m_inputClear);
            ImGui::SameLine();
            if (ImGui::Button("Clear Output"))
            {
                std::lock_guard<std::recursive_mutex> _{ m_outputLock };
                m_outputLines.clear();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Scroll Output", &m_outputShouldScroll);
            ImGui::SameLine();
            if (ImGui::Button("Shut down"))
                Options::Get().Uninject = true;
            ImGui::SameLine();
            ImGui::Checkbox("Imgui Demo", &m_Imgui_Demo);

            if (ImGui::TreeNode("Config"))
            {
                ImGuiIO& io = ImGui::GetIO();

                ImGui::CheckboxFlags("Nav EnableKey board", (unsigned int*)&io.ConfigFlags, ImGuiConfigFlags_NavEnableKeyboard);

                ImGui::CheckboxFlags("Nav Enable Gamepad", (unsigned int*)&io.ConfigFlags, ImGuiConfigFlags_NavEnableGamepad);

                ImGui::CheckboxFlags("Nav Set Mouse Pos", (unsigned int*)&io.ConfigFlags, ImGuiConfigFlags_NavEnableSetMousePos);
                
                io.MouseDrawCursor = true;

                //ImGui::CheckboxFlags("io.ConfigFlags: Nav EnableKey board", &io.ConfigFlags, ImGuiBackendFlags_HasGamepad);

                ImGui::TreePop();
            }

            static char command[200000] = { 0 };

            {
                std::lock_guard<std::recursive_mutex> _{ m_outputLock };

                ImVec2 listboxSize = ImGui::GetContentRegionAvail();
                listboxSize.y -= ImGui::GetFrameHeightWithSpacing();
                const auto result = ImGui::ListBoxHeader("", listboxSize);
                for (auto& item : m_outputLines)
                    if (ImGui::Selectable(item.c_str()))
                    {
                        auto str = item;
                        if (item[0] == '>' && item[1] == ' ')
                            str = str.substr(2);

                        std::strncpy(command, str.c_str(), sizeof(command) - 1);
                    }

                if (m_outputScroll)
                {
                    if (m_outputShouldScroll)
                        ImGui::SetScrollHereY();
                    m_outputScroll = false;
                }
                if (result)
                    ImGui::ListBoxFooter();
            }

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            const auto execute = ImGui::InputText("", command, std::size(command), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SetItemDefaultFocus();
            if (execute)
            {
                Get().Log(std::string("> ") + command);

                Scripting::Get().ExecuteLua(command);

                if (m_inputClear)
                    std::memset(command, 0, sizeof(command));

                ImGui::SetKeyboardFocusHere();
            }
        }
        else
            ImGui::Text("Unknown version, please update your game and the mod");

        ImGui::End();

#pragma endregion
    }

    if (m_Imgui_Demo)
    {
        ImGui::ShowDemoWindow();
    }

    ImGui::Render();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT APIENTRY Overlay::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_KEYDOWN && wParam == Options::Get().ConsoleKey)
    {
        s_pOverlay->Toggle();
        return 0;
    }

    if ((GetAsyncKeyState(Options::Get().ConsoleUninjectKey) & 0x1))
    {
        Options::Get().Uninject = true;
    }

    switch (uMsg)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wParam == Options::Get().ConsoleKey)
            return 0;
        break;
    case WM_CHAR:
        if (Options::Get().ConsoleChar && wParam == Options::Get().ConsoleChar)
            return 0;
        break;
    }

    if (s_pOverlay->IsEnabled() || s_pOverlay->m_Imgui_Demo)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam))
            return 1;

        // ignore mouse & keyboard events
        if ((uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) ||
            (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST))
            return 0;

        // ignore specific messages
        switch (uMsg)
        {
        case WM_INPUT:
            return 0;
        }
    }

    return CallWindowProc(s_pOverlay->m_wndProc, hwnd, uMsg, wParam, lParam);
}

struct ScriptContext
{
};

struct ScriptStack
{
    uint8_t* m_code;
    uint8_t pad[0x28];
    void* unk30;
    void* unk38;
    ScriptContext* m_context;
};
static_assert(offsetof(ScriptStack, m_context) == 0x40);

TScriptCall** GetScriptCallArray()
{
    static uint8_t* pLocation = FindSignature({ 0x4C, 0x8D, 0x15, 0xCC, 0xCC, 0xCC, 0xCC, 0x48, 0x89, 0x42, 0x38, 0x49, 0x8B, 0xF8, 0x48, 0x8B, 0x02, 0x4C, 0x8D, 0x44, 0x24, 0x20, 0xC7 }) + 3;
    static uintptr_t finalLocation = (uintptr_t)pLocation + 4 + *reinterpret_cast<uint32_t*>(pLocation);

    return reinterpret_cast<TScriptCall**>(finalLocation);
}

void Overlay::HookLog(ScriptContext* apContext, ScriptStack* apStack, void*, void*)
{
    RED4ext::REDreverse::CString text("");
    apStack->unk30 = nullptr;
    apStack->unk38 = nullptr;
    auto opcode = *(apStack->m_code++);
    GetScriptCallArray()[opcode](apStack->m_context, apStack, &text, nullptr);
    apStack->m_code++; // skip ParamEnd

    Get().Log(text.ToString());

    text.Destroy();
}

const char* GetChannelStr(uint64_t hash)
{
    switch (hash)
    {
#define HASH_CASE(x) case RED4ext::FNV1a(x): return x
        HASH_CASE("AI");
        HASH_CASE("AICover");
        HASH_CASE("ASSERT");
        HASH_CASE("Damage");
        HASH_CASE("DevelopmentManager");
        HASH_CASE("Device");
        HASH_CASE("Items");
        HASH_CASE("ItemManager");
        HASH_CASE("Puppet");
        HASH_CASE("Scanner");
        HASH_CASE("Stats");
        HASH_CASE("StatPools");
        HASH_CASE("Strike");
        HASH_CASE("TargetManager");
        HASH_CASE("Test");
        HASH_CASE("UI");
        HASH_CASE("Vehicles");
#undef HASH_CASE
    }
    return nullptr;
}

void Overlay::HookLogChannel(ScriptContext* apContext, ScriptStack* apStack, void*, void*)
{
    uint8_t opcode;

    uint64_t channel_hash = 0;
    apStack->unk30 = nullptr;
    apStack->unk38 = nullptr;
    opcode = *(apStack->m_code++);
    GetScriptCallArray()[opcode](apStack->m_context, apStack, &channel_hash, nullptr);

    RED4ext::REDreverse::CString text("");
    apStack->unk30 = nullptr;
    apStack->unk38 = nullptr;
    opcode = *(apStack->m_code++);
    GetScriptCallArray()[opcode](apStack->m_context, apStack, &text, nullptr);

    apStack->m_code++; // skip ParamEnd

    auto channel_str = GetChannelStr(channel_hash);
    std::string channel = channel_str == nullptr
        ? "?" + std::to_string(channel_hash)
        : std::string(channel_str);
    Get().Log("[" + channel + "] " +text.ToString());

    text.Destroy();
}

std::string GetTDBDIDDebugString(TDBID tdbid)
{
    return (tdbid.unk5 == 0 && tdbid.unk7 == 0)
        ? fmt::format("<TDBID:{:08X}:{:02X}>",
            tdbid.name_hash, tdbid.name_length)
        : fmt::format("<TDBID:{:08X}:{:02X}:{:04X}:{:02X}>",
            tdbid.name_hash, tdbid.name_length, tdbid.unk5, tdbid.unk7);
}

void Overlay::RegisterTDBIDString(uint64_t value, uint64_t base, const std::string& name)
{
    std::lock_guard<std::recursive_mutex> _{ m_tdbidLock };
    m_tdbidLookup[value] = { base, name };
}

std::string Overlay::GetTDBIDString(uint64_t value)
{
    std::lock_guard<std::recursive_mutex> _{ m_tdbidLock };
    auto it = m_tdbidLookup.find(value);
    auto end = Get().m_tdbidLookup.end();
    if (it == end)
        return GetTDBDIDDebugString(TDBID{ value });
    std::string string = (*it).second.name;
    uint64_t base = (*it).second.base;
    while (base)
    {
        it = m_tdbidLookup.find(base);
        if (it == end)
        {
            string.insert(0, GetTDBDIDDebugString(TDBID{ base }));
            break;
        }
        string.insert(0, (*it).second.name);
        base = (*it).second.base;
    }
    return string;
}

TDBID* Overlay::HookTDBIDCtor(TDBID* apThis, const char* name)
{
    auto result = Get().m_realTDBIDCtor(apThis, name);
    Get().RegisterTDBIDString(apThis->value, 0, name);
    return result;
}

TDBID* Overlay::HookTDBIDCtorCString(TDBID* apThis, const RED4ext::REDreverse::CString* name)
{
    auto result = Get().m_realTDBIDCtorCString(apThis, name);
    Get().RegisterTDBIDString(apThis->value, 0, name->ToString());
    return result;
}

TDBID* Overlay::HookTDBIDCtorDerive(TDBID* apBase, TDBID* apThis, const char* name)
{
    auto result = Get().m_realTDBIDCtorDerive(apBase, apThis, name);
    Get().RegisterTDBIDString(apThis->value, apBase->value, std::string(name));
    return result;
}

struct UnknownString
{
    const char* string;
    uint32_t size;
};

TDBID* Overlay::HookTDBIDCtorUnknown(TDBID* apThis, uint64_t name)
{
    auto result = Get().m_realTDBIDCtorUnknown(apThis, name);
    UnknownString unknown;
    Get().m_someStringLookup(&name, &unknown);
    Get().RegisterTDBIDString(apThis->value, 0, std::string(unknown.string, unknown.size));
    return result;
}

void Overlay::HookTDBIDToStringDEBUG(ScriptContext* apContext, ScriptStack* apStack, void* result, void*)
{
    uint8_t opcode;

    TDBID tdbid;
    apStack->unk30 = nullptr;
    apStack->unk38 = nullptr;
    opcode = *(apStack->m_code++);
    GetScriptCallArray()[opcode](apStack->m_context, apStack, &tdbid, nullptr);
    apStack->m_code++; // skip ParamEnd

    if (result)
    {
        std::string name = Get().GetTDBIDString(tdbid.value);
        RED4ext::REDreverse::CString s(name.c_str());
        static_cast<RED4ext::REDreverse::CString*>(result)->Copy(&s);
        s.Destroy();
    }
}

void Overlay::Toggle()
{
    struct Singleton
    {
        uint8_t pad0[0xC0];
        SomeStruct* pSomeStruct;
    };

    m_enabled = !m_enabled;

    while(true)
    {
        if (m_enabled && ShowCursor(TRUE) >= 0)
            break;
        if (!m_enabled && ShowCursor(FALSE) < 0)
            break;
    }

    ClipToCenter(CGameEngine::Get()->pSomeStruct);
}

bool Overlay::IsEnabled() const
{
    return m_enabled;
}

void Overlay::Log(const std::string& acpText)
{
    std::lock_guard<std::recursive_mutex> _{ m_outputLock };
    m_outputLines.emplace_back(acpText);
    m_outputScroll = true;
}

Overlay::Overlay() = default;

Overlay::~Overlay() = default;

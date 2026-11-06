#include "stdafx.h"

// Floating windows never stay out of sight. Docking is on and viewports are off, so an undocked
// window (a tab dragged out) is an ordinary ImGui window inside the main window: it keeps the
// position it was dropped at, and when the main window shrinks, or a window was left past its edge,
// nothing moved it back: it was still there, drawn outside what the window shows, until the window was
// made larger again. KeepWindowsVisible puts such a window back inside, every frame, and
// HiddenWindows is the check a screenshot run makes (ui-shot fails when a window is out of sight).
#include "ImGuiShell.Internal.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ShellUi
{
    namespace
    {
        // a floating, movable window of the program's own: not a child, a popup, a tooltip, a docked
        // tab, a bar fixed to an edge or the overlays that take no input
        // Previous: asked at the start of a frame, before the windows have been submitted again (Active is
        // cleared by NewFrame: WasActive is the last frame's)
        bool Floats(const ImGuiWindow* W, bool Previous)
        {
            if (W == nullptr || !(Previous ? W->WasActive : W->Active) || W->Hidden) return false;
            if (std::strncmp(W->Name, "Debug##", 7) == 0) return false;   // ImGui's own empty fallback window
            constexpr ImGuiWindowFlags Not = ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip |
                                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_ChildMenu;
            if ((W->Flags & Not) != 0) return false;
            return W->DockNode == nullptr && !W->DockIsActive && W->ParentWindow == nullptr;
        }

        // how much of a window must stay inside: enough of its title bar to grab it
        ImVec2 MustShow(const ImGuiWindow* W) { return ImVec2(std::min(W->Size.x, 120.0f), std::min(W->Size.y, 36.0f)); }
    }

    std::vector<std::string> HiddenWindows()
    {
        std::vector<std::string> Out;
        const ImGuiViewport* Vp = ImGui::GetMainViewport();
        const ImVec2 Min = Vp->WorkPos, Max(Vp->WorkPos.x + Vp->WorkSize.x, Vp->WorkPos.y + Vp->WorkSize.y);
        for (const ImGuiWindow* W : ImGui::GetCurrentContext()->Windows)
        {
            if (!Floats(W, false)) continue;
            const ImVec2 Need = MustShow(W);
            const float VisibleW = std::min(W->Pos.x + W->Size.x, Max.x) - std::max(W->Pos.x, Min.x);
            const float VisibleH = std::min(W->Pos.y + W->Size.y, Max.y) - std::max(W->Pos.y, Min.y);
            if (VisibleW < Need.x - 0.5f || VisibleH < Need.y - 0.5f) Out.emplace_back(W->Name);
        }
        return Out;
    }

    void KeepWindowsVisible()
    {
        static const bool Off = [] { char V[4] = {}; return GetEnvironmentVariableA("GRAYWOLF_NO_WINDOW_CLAMP", V, sizeof(V)) > 0; }();   // the test's negative control
        if (Off) return;
        const ImGuiViewport* Vp = ImGui::GetMainViewport();
        const ImVec2 Min = Vp->WorkPos, Max(Vp->WorkPos.x + Vp->WorkSize.x, Vp->WorkPos.y + Vp->WorkSize.y);
        for (ImGuiWindow* W : ImGui::GetCurrentContext()->Windows)
        {
            if (!Floats(W, true)) continue;
            // wholly inside the work area when it fits (else its top left corner is), and no larger than it
            const ImVec2 WorkSize(Max.x - Min.x, Max.y - Min.y);
            ImVec2 Size = W->Size;
            Size.x = std::min(Size.x, WorkSize.x); Size.y = std::min(Size.y, WorkSize.y);
            ImVec2 Pos = W->Pos;
            Pos.x = std::clamp(Pos.x, Min.x, std::max(Min.x, Max.x - Size.x));
            Pos.y = std::clamp(Pos.y, Min.y, std::max(Min.y, Max.y - Size.y));
            if (Size.x != W->Size.x || Size.y != W->Size.y) ImGui::SetWindowSize(W, Size, ImGuiCond_Always);
            if (Pos.x != W->Pos.x || Pos.y != W->Pos.y)
            {
                ImGui::SetWindowPos(W, Pos, ImGuiCond_Always);
                ImGui::FocusWindow(W);   // and in front, as a window that has just come back
            }
        }
    }
}

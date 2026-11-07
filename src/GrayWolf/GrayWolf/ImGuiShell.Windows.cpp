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
#include <map>
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
            // modals are left alone, and so are the hosts of a dock node and a window about to be docked
            if ((W->Flags & (ImGuiWindowFlags_Modal | ImGuiWindowFlags_DockNodeHost)) != 0) return false;
            if (W->DockId != 0 || W->DockNodeAsHost != nullptr) return false;
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

    ImVec2 FittedSize(const ImVec2& Wanted, const ImVec2& WorkMin, const ImVec2& WorkMax)
    {
        return ImVec2(std::min(Wanted.x, WorkMax.x - WorkMin.x), std::min(Wanted.y, WorkMax.y - WorkMin.y));
    }

    WindowPlan PlanWindow(const ImVec2& Pos, const ImVec2& Size, const ImVec2& Wanted, const ImVec2& WorkMin, const ImVec2& WorkMax, bool Dragging)
    {
        WindowPlan Plan{ Pos, Size };
        // a minimised or tiny main window has no room for anything: leave every window alone, so that
        // nothing is squeezed to nothing and kept so (the work area is a few pixels while minimised)
        if (WorkMax.x - WorkMin.x < 160.0f || WorkMax.y - WorkMin.y < 120.0f) return Plan;
        // never smaller for good: the size the window was given (Wanted) comes back when there is room
        Plan.Size = FittedSize(Wanted, WorkMin, WorkMax);
        // a drag in progress is not fought: the window goes where the pointer takes it, and is brought
        // back when it is let go
        if (Dragging) return Plan;
        Plan.Pos.x = std::clamp(Pos.x, WorkMin.x, std::max(WorkMin.x, WorkMax.x - Plan.Size.x));
        Plan.Pos.y = std::clamp(Pos.y, WorkMin.y, std::max(WorkMin.y, WorkMax.y - Plan.Size.y));
        return Plan;
    }

    void KeepWindowsVisible()
    {
#ifdef GW_TEST_HOOKS
        static const bool Off = [] { char V[4] = {}; return GetEnvironmentVariableA("GRAYWOLF_NO_WINDOW_CLAMP", V, sizeof(V)) > 0; }();   // the test's negative control (Dev builds only)
        if (Off) return;
#endif
        // per window: the size it was last given by the user (Wanted) and the size this function set (Applied)
        struct Sizes { ImVec2 Wanted, Applied; };
        static std::map<ImGuiID, Sizes> Known;
        const ImGuiViewport* Vp = ImGui::GetMainViewport();
        const ImVec2 Min = Vp->WorkPos, Max(Vp->WorkPos.x + Vp->WorkSize.x, Vp->WorkPos.y + Vp->WorkSize.y);
        ImGuiContext& G = *ImGui::GetCurrentContext();
        for (ImGuiWindow* W : G.Windows)
        {
            if (!Floats(W, true)) continue;
            Sizes& K = Known.try_emplace(W->ID, Sizes{ W->Size, W->Size }).first->second;
            if (W->Size.x != K.Applied.x || W->Size.y != K.Applied.y) K.Wanted = W->Size;   // resized by the user or by its content
            const bool Dragging = G.MovingWindow != nullptr && G.MovingWindow->RootWindow == W->RootWindow;
            const WindowPlan Plan = PlanWindow(W->Pos, W->Size, K.Wanted, Min, Max, Dragging);
            K.Applied = Plan.Size;
            if (Plan.Size.x != W->Size.x || Plan.Size.y != W->Size.y) ImGui::SetWindowSize(W, Plan.Size, ImGuiCond_Always);
            if (Plan.Pos.x != W->Pos.x || Plan.Pos.y != W->Pos.y) ImGui::SetWindowPos(W, Plan.Pos, ImGuiCond_Always);   // not focused: it is moved, not summoned
        }
    }

    bool WindowSelfTest(std::string& Report)
    {
        bool Ok = true;
        const auto Fail = [&](const char* What) { Ok = false; Report += std::string("FAIL windows: ") + What + "\n"; };
        const ImVec2 Min(0, 20), Max(1000, 700);
        // off the edge and larger than the area: back inside, smaller
        WindowPlan P = PlanWindow(ImVec2(2000, -300), ImVec2(1200, 900), ImVec2(1200, 900), Min, Max, false);
        if (P.Pos.x != 0 || P.Pos.y != 20 || P.Size.x != 1000 || P.Size.y != 680) Fail("an out-of-sight window is brought inside");
        // the same while dragging: moved by the pointer, not by us (the size still fits)
        P = PlanWindow(ImVec2(2000, -300), ImVec2(400, 300), ImVec2(400, 300), Min, Max, true);
        if (P.Pos.x != 2000 || P.Pos.y != -300) Fail("a drag in progress is not fought");
        P = PlanWindow(ImVec2(2000, -300), ImVec2(400, 300), ImVec2(400, 300), Min, Max, false);
        if (P.Pos.x != 600 || P.Pos.y != 20) Fail("a window let go off the edge is brought back");
        // minimised (a few pixels of work area): nothing moves, nothing shrinks
        P = PlanWindow(ImVec2(300, 200), ImVec2(400, 300), ImVec2(400, 300), ImVec2(0, 0), ImVec2(0, 0), false);
        if (P.Pos.x != 300 || P.Pos.y != 200 || P.Size.x != 400 || P.Size.y != 300) Fail("a minimised window leaves the windows alone");
        // the size comes back when the area does
        const ImVec2 Small = FittedSize(ImVec2(500, 400), ImVec2(0, 0), ImVec2(300, 250));
        const ImVec2 Back = FittedSize(ImVec2(500, 400), ImVec2(0, 0), ImVec2(1200, 800));
        if (Small.x != 300 || Small.y != 250 || Back.x != 500 || Back.y != 400) Fail("a shrunk window gets its size back");
        return Ok;
    }
}

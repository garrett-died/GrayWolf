#include "stdafx.h"

// The shell's frame around its panels: the main menu bar, the status bar, the dock layout, the filter help.
#include "ImGuiShell.Internal.h"
#include "AppLog.h"           // session log, verbose
#include "UiAsync.h"           // shell opens off the frame
#include "Strings.h"
#include "SettingsManager.h"   // exportsnd
#include "ImGuiUpdates.h"      // the new-version banner

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder* for the initial layout

#include <algorithm>

namespace ShellUi
{
    bool DrawMainMenuBar(bool& OpenHelp, bool& OpenSettings)
    {
        bool Quit = false;
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File")) { if (ImGui::MenuItem("Exit")) Quit = true; ImGui::EndMenu(); }
            // one place for every setting: a page, not a menu (the former Settings menus, the Options popup)
            if (ImGui::MenuItem("Settings")) OpenSettings = true;
            if (ImGui::BeginMenu("Help"))
            {
                if (ImGui::MenuItem("Search syntax")) OpenHelp = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Open the session log")) UiAsync::OpenInShell(AppLog::SessionPath());
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
        // a newer GrayWolf: Download / Later, then Restart now (the helper replaces the files)
        if (DrawUpdateBanner()) Quit = true;
        return Quit;
    }

    void DrawWorkflowBar(ShellState& S)
    {
        ImGuiViewport* Vp = ImGui::GetMainViewport();
        if (ImGui::BeginViewportSideBar("##workflow", Vp, ImGuiDir_Up, ImGui::GetFrameHeight() * 1.35f,
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar))
        {
            if (ImGui::BeginMenuBar())
            {
                const struct { const char* Label; const char* Tip; } Steps[] = {
                    { "1  Extract from a game", "Load a game, search its assets, pick what you want and press Export" },
                    { "2  Library: browse and preview", "Your exports: search, filter, click an asset to preview it, Ctrl+click to view several together" },
                };
                for (int i = 0; i < 2; i++)
                {
                    if (Theme::Pill(Steps[i].Label, S.LeftPage == i)) S.LeftPage = i;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Steps[i].Tip);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("\xC2\xB7  then click an asset to preview it on the right");
                ImGui::EndMenuBar();
            }
        }
        ImGui::End();
    }

    // Bottom status bar (trims the work area before the dockspace claims the rest). Uses the
    // menu-bar idiom so the text is vertically centred in the frame height and never clipped.
    void DrawStatusBar(const ShellState& S)
    {
        const auto& Models = S.Catalog.Models;
        ImGuiViewport* MainVp = ImGui::GetMainViewport();
        if (ImGui::BeginViewportSideBar("##status", MainVp, ImGuiDir_Down, ImGui::GetFrameHeight(),
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar))
        {
            if (ImGui::BeginMenuBar())
            {
                ImGui::Text("%d assets", (int)Models.size());
                ImGui::TextDisabled("|");
                if (S.Selected >= 0 && S.Selected < (int)Models.size()) ImGui::Text("Selected: %s", Models[S.Selected].Name.c_str());
                else ImGui::TextDisabled("No selection");
                const char* Hint = "drag: orbit   \xC2\xB7   middle drag: pan   \xC2\xB7   wheel: zoom";
                ImGui::SameLine(std::max(0.0f, ImGui::GetWindowWidth() - ImGui::CalcTextSize(Hint).x - 14.0f));
                ImGui::TextDisabled("%s", Hint);
                ImGui::EndMenuBar();
            }
        }
        ImGui::End();
    }

    // First frame: split the dockspace so Extract and Library fill the left and Viewport the rest, all
    // filling the window (otherwise ImGui leaves them as small floating windows in the corner).
    void BuildDockLayout(unsigned int DockId)
    {
        static bool LayoutBuilt = false;
        if (LayoutBuilt) return;
        LayoutBuilt = true;
        ImGui::DockBuilderRemoveNode(DockId);
        ImGui::DockBuilderAddNode(DockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(DockId, ImGui::GetMainViewport()->Size);
        ImGuiID Left = 0, Right = 0;
        ImGui::DockBuilderSplitNode(DockId, ImGuiDir_Left, 0.38f, &Left, &Right);
        // Tabbed together: reading a game and browsing what was exported are the same job seen
        // from two ends, and they never need to be visible at once.
        ImGui::DockBuilderDockWindow(Theme::ExtractWindow, Left);
        ImGui::DockBuilderDockWindow(Theme::LibraryWindow, Left);
        ImGui::DockBuilderDockWindow("Viewport", Right);
        ImGui::DockBuilderFinish(DockId);
    }

    // Filter help dialog (opened from the '?' button or the Help menu)
    void DrawFilterHelp(bool OpenHelp)
    {
        if (OpenHelp) ImGui::OpenPopup("Search syntax");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Search syntax", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Search matches the asset name, or a column");
            ImGui::Separator();
            ImGui::BulletText("space   =   AND  (every term must appear)");
            ImGui::BulletText("!term   =   the term must NOT appear");
            ImGui::BulletText("key:term   =   look in one column: name, type, game, format");
            ImGui::BulletText("\"term\"   =   the whole value, not part of it");
            ImGui::BulletText("several \"exact\" terms of one key   =   any of them");
            ImGui::BulletText("case-insensitive");
            ImGui::Spacing(); ImGui::TextDisabled("Examples");
            ImGui::BulletText("richtofen              names containing \"richtofen\"");
            ImGui::BulletText("zombie !head           \"zombie\" and not \"head\"");
            ImGui::BulletText("type:image game:bo3    images of one game");
            ImGui::BulletText("type:\"model\" type:\"anim\" crate   models or anims named like crate");
            ImGui::Spacing();
            ImGui::TextDisabled("Tip: sort by any column by clicking its header.");
            ImGui::TextDisabled("Number ranges (bones, meshes, materials, size) are under Filters in the toolbar.");
            ImGui::Spacing();
            if (ImGui::Button("Got it", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
}

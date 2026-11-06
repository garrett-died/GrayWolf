#include "stdafx.h"

#include "ImGuiUpdates.h"

#include "AppLog.h"
#include "SettingsManager.h"
#include "Updater.h"

#include "imgui.h"
#include "imgui_internal.h" // BeginViewportSideBar

namespace
{
    bool BannerDismissed = false;
    std::string ApplyError;

    bool SettingOn(const char* Key, const char* Default) { return SettingsManager::GetSetting(Key, Default) == "true"; }
}

void DrawUpdatesSettings()
{
    bool Auto = SettingOn("auto_update", "true");
    if (ImGui::Checkbox("Check for updates when GrayWolf starts", &Auto))
        SettingsManager::SetSetting("auto_update", Auto ? "true" : "false");
    bool Gh = SettingOn("update_use_gh_token", "false");
    if (ImGui::Checkbox("Use the GitHub CLI's token for the check", &Gh))
        SettingsManager::SetSetting("update_use_gh_token", Gh ? "true" : "false");
    ImGui::Spacing();
    const Updater::Background::View V = Updater::Background::Get();
    ImGui::BeginDisabled(V.Busy);
    if (ImGui::Button(V.Busy ? "Checking\xE2\x80\xA6" : "Check for updates now"))
    {
        BannerDismissed = false;
        Updater::Background::Start(false);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Installed version: %s", Updater::AppVersion().c_str());
    ImGui::TextDisabled("Repository: %s", SettingsManager::GetSetting("update_repo", "Kenshin9977/GrayWolf").c_str());
    if (!V.Line.empty()) ImGui::TextDisabled("%s", V.Line.c_str());
}

bool DrawUpdateBanner()
{
    const Updater::Background::View V = Updater::Background::Get();
    if (V.AppVersion.empty() || BannerDismissed) return false;

    bool Restart = false;
    ImGuiViewport* Vp = ImGui::GetMainViewport();
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.20f, 0.33f, 0.50f, 1.0f));
    if (ImGui::BeginViewportSideBar("##update", Vp, ImGuiDir_Up, ImGui::GetFrameHeight(),
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar))
    {
        if (ImGui::BeginMenuBar())
        {
            if (!ApplyError.empty())
                ImGui::Text("Update failed to start: %s", ApplyError.c_str());
            else if (V.AppStaged)
                ImGui::Text("GrayWolf %s is downloaded and verified.", V.AppVersion.c_str());
            else if (V.Busy)
                ImGui::Text("Downloading GrayWolf %s\xE2\x80\xA6", V.AppVersion.c_str());
            else if (!V.Error.empty())
                ImGui::Text("Download failed: %s", V.Error.c_str());
            else
                ImGui::Text("GrayWolf %s is available (you have %s).", V.AppVersion.c_str(), Updater::AppVersion().c_str());

            if (V.AppStaged && ApplyError.empty())
            {
                if (ImGui::SmallButton("Restart now"))
                {
                    std::string Msg;
                    if (Updater::Background::ApplyAndRestart(Msg)) Restart = true;
                    else ApplyError = Msg;
                }
            }
            else if (!V.Busy && !V.AppStaged)
            {
                if (ImGui::SmallButton(V.Error.empty() ? "Download" : "Retry")) Updater::Background::DownloadApp();
            }
            if (ImGui::SmallButton("Later")) BannerDismissed = true;
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    return Restart;
}

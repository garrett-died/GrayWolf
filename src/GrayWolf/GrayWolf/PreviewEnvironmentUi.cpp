#include "stdafx.h"

#include "PreviewEnvironmentUi.h"
#include "PreviewEnvironment.h"
#include "ModelPreviewGPU.h"   // PreviewLighting
#include "SettingsManager.h"
#include "SkyLighting.h"
#include "PreviewToolbar.h"
#include "ImGuiTheme.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <utility>
#include <string>

namespace
{
    void Save(const PreviewLighting& L)
    {
        // a baked sky's id is not kept (its hash changes with the files): the state is, as preview_light (SkyLighting.h)
        if (L.Environment.rfind("sky-", 0) != 0) SettingsManager::SetSetting("preview_environment", L.Environment);
        SettingsManager::SetSetting("preview_env_rotation", std::to_string(L.EnvRotation));
        SettingsManager::SetSetting("preview_env_intensity", std::to_string(L.EnvIntensity));
        SettingsManager::SetSetting("preview_bloom", L.Bloom ? "true" : "false");
        SettingsManager::SetSetting("preview_bloom_strength", std::to_string(L.BloomStrength));
        SettingsManager::SetSetting("preview_env_backdrop", L.Backdrop ? "true" : "false");
        SettingsManager::SetSetting("preview_env_backdrop_blur", std::to_string(L.BackdropBlur));
    }

    const PreviewEnvironmentInfo* Find(const std::string& Id)
    {
        for (const auto& E : PreviewEnvironments()) if (E.Id == Id) return &E;
        return nullptr;
    }
}

void LoadPreviewEnvironmentSettings(PreviewLighting& L)
{
    const PreviewLighting Default;
    L.Environment = SettingsManager::GetSetting("preview_environment", Default.Environment);
    // a baked one no longer shipped: the default, else the procedural sky (the default's files missing)
    // a baked one no longer shipped: the default, else the procedural sky (the default's files missing)
    if (!Find(L.Environment)) L.Environment = Find(Default.Environment) ? Default.Environment : std::string(kStudioEnvironment);
    L.EnvRotation = SettingsManager::GetSettingFloat("preview_env_rotation", Default.EnvRotation, -180.0f, 180.0f);
    L.EnvIntensity = SettingsManager::GetSettingFloat("preview_env_intensity", Default.EnvIntensity, 0.0f, 4.0f);
    L.Bloom = SettingsManager::GetSetting("preview_bloom", Default.Bloom ? "true" : "false") == "true";
    L.BloomStrength = SettingsManager::GetSettingFloat("preview_bloom_strength", Default.BloomStrength, 0.0f, 2.0f);
    L.Backdrop = SettingsManager::GetSetting("preview_env_backdrop", Default.Backdrop ? "true" : "false") == "true";
    L.BackdropBlur = SettingsManager::GetSettingFloat("preview_env_backdrop_blur", Default.BackdropBlur, 0.0f, 1.0f);
}

namespace
{
    int LightingMenuRequest = 0;   // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): ui-shot's one-shot request, set from the recorder and read by the toolbar (UI thread); 1: open, 2: close
}

void RequestLightingMenu(bool Open) { LightingMenuRequest = Open ? 1 : 2; }

void DrawPreviewEnvironmentControls(PreviewLighting& L, SkyLighting::Service& Sky, float (&Background)[3])
{
    const PreviewLighting Default;
    const float Em = ImGui::GetFontSize();
    const SkyLighting::Menu Menu = Sky.GetMenu();

    // ---- the Lighting menu: the four BO3 skies, Fullbright, the environments that ship
    const bool LightingClicked = ImGui::Button((Sky.Label(L) + "##lighting").c_str(), ImVec2(Em * 6.0f, 0));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lighting: a Black Ops III sky from the Mod Tools, Fullbright, or one of GrayWolf's environments");
    const ImVec2 MenuAt(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);   // the menu opens under its button
    if (LightingClicked) ImGui::OpenPopup("lightingmenu");
    const int Request = std::exchange(LightingMenuRequest, 0);   // ui-shot: shown open or closed
    if (Request == 1) ImGui::OpenPopup("lightingmenu");
    ImGui::SetNextWindowPos(MenuAt, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("lightingmenu"))
    {
        if (Request == 2) ImGui::CloseCurrentPopup();
        ImGui::TextDisabled("Black Ops III skies");
        for (const SkyLighting::StateRow& R : Menu.States)
        {
            std::string Text = R.Label;
            if (R.Preparing) Text += "  (preparing " + std::to_string((int)std::lround(R.Progress * 100.0f)) + "%)";
            ImGui::BeginDisabled(!R.Enabled);
            if (ImGui::Selectable((Text + "##state" + std::to_string((int)R.State)).c_str(), R.Checked)) Sky.PickState(R.State, L);
            ImGui::EndDisabled();
            if (!R.Enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", R.Reason.c_str());
        }
        if (!Menu.NoToolsLine.empty())
        {
            ImGui::PushTextWrapPos(Em * 22.0f);
            ImGui::TextDisabled("%s", Menu.NoToolsLine.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Separator();
        if (ImGui::Selectable("Fullbright##fullbright", Menu.FullbrightChecked)) Sky.SetFullbright(!Menu.FullbrightChecked, L);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("No lights: every surface shows its texture colour. Your other lighting is kept, so switching it off brings the look back.");
        ImGui::Separator();
        ImGui::TextDisabled("Environments");
        const SkyLighting::Choice Chosen = Sky.Current();
        for (const auto& E : PreviewEnvironments())
            if (ImGui::Selectable((E.Name + "##env" + E.Id).c_str(), !Chosen.Sky && !Chosen.Fullbright && E.Id == L.Environment))
            {
                Sky.PickEnvironment(E.Id, L);
                Save(L);
            }
        ImGui::EndPopup();
    }
    for (const SkyLighting::StateRow& R : Menu.States)
        if (R.Preparing)
        {
            PreviewToolbar::SameLine(Em * 0.3f, Em * 6.0f);
            char Text[48];
            std::snprintf(Text, sizeof(Text), "%s %d%%", R.Label.c_str(), (int)std::lround(R.Progress * 100.0f));
            ImGui::ProgressBar(R.Progress, ImVec2(Em * 6.0f, 0), Text);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preparing this sky for the preview (once; it is kept). The current look stays until it is ready.");
        }

    // ---- the sky chip: the sky behind the model, or a background colour
    PreviewToolbar::SameLine(Em * 0.3f, Em * 3.0f);
    if (Theme::Pill("Sky", L.Backdrop)) { L.Backdrop = !L.Backdrop; Save(L); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The sky seen behind the model. Off: a plain background colour (screenshots stay transparent either way)");
    if (!L.Backdrop)
    {
        static bool ColorDirty = false;
        PreviewToolbar::SameLine(Em * 0.2f, Em * 1.3f);
        const ImVec4 Swatch(Background[0], Background[1], Background[2], 1.0f);
        if (ImGui::ColorButton("##background", Swatch, ImGuiColorEditFlags_NoTooltip, ImVec2(Em * 1.3f, Em * 1.3f))) ImGui::OpenPopup("backgroundflyout");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Background colour");
        if (ImGui::BeginPopup("backgroundflyout"))
        {
            ImGui::TextUnformatted("Background colour");
            if (ImGui::ColorPicker3("##bgpicker", Background, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoAlpha |
                                                              ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_InputRGB)) ColorDirty = true;
            if (ImGui::Button("Default grey"))
            {
                for (int I = 0; I < 3; I++) Background[I] = kPreviewBackground[I];
                ColorDirty = true;
            }
            ImGui::EndPopup();
        }
        else if (ColorDirty)
        {
            ColorDirty = false;
            SettingsManager::SetSetting(SkyLighting::kBackgroundKey, SkyLighting::FormatColor(Background));
        }
    }

    PreviewToolbar::SameLine(ImGui::GetFontSize() * 0.2f, ImGui::GetFontSize() * 2.2f);
    if (Theme::IconButton("envsettings", Theme::Icon::Sliders, "Environment turn and strength, backdrop, bloom", L.Bloom))
        ImGui::OpenPopup("envpopup");
    if (ImGui::BeginPopup("envpopup"))
    {
        const float W = ImGui::GetFontSize() * 11.0f;
        const auto Row = [&](const char* Label, const char* Id, float* V, float Lo, float Hi, const char* Fmt, float Reset, const char* Tip) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Label);
            ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
            ImGui::SetNextItemWidth(W);
            ImGui::SliderFloat(Id, V, Lo, Hi, Fmt);
            if (ImGui::IsItemDeactivatedAfterEdit()) Save(L);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (double-click to type, right-click resets)", Tip);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { *V = Reset; Save(L); }
        };
        Row("Turn", "##envturn", &L.EnvRotation, -180.0f, 180.0f, "%.0f\xC2\xB0", Default.EnvRotation, "The environment turned about the up axis");
        Row("Strength", "##envstrength", &L.EnvIntensity, 0.0f, 4.0f, "%.2fx", Default.EnvIntensity, "The environment's light");
        ImGui::Separator();
        if (ImGui::Checkbox("Backdrop", &L.Backdrop)) Save(L);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The environment shown behind the model, blurred (screenshots stay transparent)");
        ImGui::BeginDisabled(!L.Backdrop);
        Row("Blur", "##backdropblur", &L.BackdropBlur, 0.0f, 1.0f, "%.2f", Default.BackdropBlur, "How blurred the backdrop is");
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::Checkbox("Bloom", &L.Bloom)) Save(L);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A soft glow around the brightest parts (highlights, emission)");
        ImGui::BeginDisabled(!L.Bloom);
        Row("Amount", "##bloomstrength", &L.BloomStrength, 0.0f, 2.0f, "%.2f", Default.BloomStrength, "The glow's strength");
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}

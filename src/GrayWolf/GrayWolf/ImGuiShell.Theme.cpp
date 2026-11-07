#include "stdafx.h"

// The theme (ImGuiTheme.h): fonts, colours, and the widgets every panel draws alike.
#include "ImGuiTheme.h"

#include "SettingsManager.h"

#include <windows.h>
#include <algorithm>
#include <iterator>

namespace Theme
{
    namespace
    {
        float Scale = 1.0f;

        bool Exists(const wchar_t* Path) { return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES; }

        // A Segoe UI face at Size, then the icons and CODOL's Chinese merged into it
        ImFont* AddFace(const char* File, const wchar_t* WFile, float Size)
        {
            ImGuiIO& io = ImGui::GetIO();
            if (!Exists(WFile)) return nullptr;
            ImFontConfig Fc; Fc.OversampleH = 2; Fc.OversampleV = 2;
            ImFont* F = io.Fonts->AddFontFromFileTTF(File, Size, &Fc);
            if (!F) return nullptr;
            if (Exists(L"C:\\Windows\\Fonts\\segmdl2.ttf"))
            {
                // the icons sit on the text's line, a little smaller than its cap height, each as wide as tall
                static const ImWchar Icons[] = { 0xE700, 0xF8FF, 0 };
                ImFontConfig Ic; Ic.MergeMode = true; Ic.GlyphOffset = ImVec2(0, Size * 0.14f); Ic.GlyphMinAdvanceX = Size * 1.05f;
                io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segmdl2.ttf", Size * 0.9f, &Ic, Icons);
            }
            // the games' level names: CODOL's Chinese, from Microsoft YaHei, when the system has it
            if (Exists(L"C:\\Windows\\Fonts\\msyh.ttc"))
            {
                ImFontConfig Cjk; Cjk.MergeMode = true; Cjk.OversampleH = 1; Cjk.OversampleV = 1;
                io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", Size, &Cjk, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            }
            return F;
        }
    }

    namespace
    {
        // the theme in use (an index into Themes), -1 until the setting is read
        int& Chosen()
        {
            static int Index = -1;
            return Index;
        }

        struct ThemeInfo { const char* Key; const char* Name; const char* About; };
        const ThemeInfo Themes[] = {
            { "graywolf", "GrayWolf", "Near-black surfaces with one orange accent. The default." },
            { "hydrax", "HydraX", "The blue-grey look of the HydraX app: slate panels, a blue accent." },
        };

        ImVec4 Rgb(int R, int G, int B, float A = 1.0f) { return ImVec4(R / 255.0f, G / 255.0f, B / 255.0f, A); }

        // GrayWolf's own colours (the default): a surface per height, one accent. Every colour is a
        // literal, as before (ux/ux-check.py reads them for its contrast check).
        void ApplyGrayWolf(ImGuiStyle& St)
        {
            Colours& C = MutableColours();
            C.Base = ImVec4(0.071f, 0.075f, 0.086f, 1.0f); C.Panel = ImVec4(0.102f, 0.106f, 0.122f, 1.0f);
            C.Raised = ImVec4(0.149f, 0.153f, 0.173f, 1.0f); C.Overlay = ImVec4(0.180f, 0.184f, 0.208f, 1.0f);
            C.Text = ImVec4(0.925f, 0.929f, 0.941f, 1.0f); C.Muted = ImVec4(0.620f, 0.635f, 0.667f, 1.0f);
            C.Accent = ImVec4(0.961f, 0.647f, 0.141f, 1.0f); C.AccentHover = ImVec4(1.000f, 0.718f, 0.290f, 1.0f);
            C.AccentSoft = ImVec4(0.961f, 0.647f, 0.141f, 0.16f); C.AccentText = C.Accent; C.OnAccent = ImVec4(0.082f, 0.090f, 0.110f, 1.0f);
            C.AccentU32 = IM_COL32(245, 165, 36, 255);
            St.Colors[ImGuiCol_Text]                 = ImVec4(0.925f, 0.929f, 0.941f, 1.0f);
            St.Colors[ImGuiCol_TextDisabled]         = ImVec4(0.62f, 0.635f, 0.667f, 1.0f);
            St.Colors[ImGuiCol_WindowBg]             = ImVec4(0.102f, 0.106f, 0.122f, 1.0f);
            St.Colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            St.Colors[ImGuiCol_PopupBg]              = ImVec4(0.18f, 0.184f, 0.208f, 0.98f);
            St.Colors[ImGuiCol_MenuBarBg]            = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
            St.Colors[ImGuiCol_Border]               = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
            St.Colors[ImGuiCol_FrameBg]              = ImVec4(0.149f, 0.153f, 0.173f, 1.0f);
            St.Colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.18f, 0.184f, 0.208f, 1.0f);
            St.Colors[ImGuiCol_FrameBgActive]        = ImVec4(0.204f, 0.208f, 0.235f, 1.0f);
            St.Colors[ImGuiCol_TitleBg]              = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
            St.Colors[ImGuiCol_TitleBgActive]        = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
            St.Colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
            St.Colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            St.Colors[ImGuiCol_ScrollbarGrab]        = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
            St.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.20f);
            St.Colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1.0f, 1.0f, 1.0f, 0.28f);
            St.Colors[ImGuiCol_CheckMark]            = ImVec4(0.961f, 0.647f, 0.141f, 1.0f);
            St.Colors[ImGuiCol_SliderGrab]           = ImVec4(1.0f, 1.0f, 1.0f, 0.28f);
            St.Colors[ImGuiCol_SliderGrabActive]     = ImVec4(1.0f, 0.718f, 0.29f, 1.0f);
            St.Colors[ImGuiCol_Button]               = ImVec4(0.149f, 0.153f, 0.173f, 1.0f);
            St.Colors[ImGuiCol_ButtonHovered]        = ImVec4(0.204f, 0.208f, 0.235f, 1.0f);
            St.Colors[ImGuiCol_ButtonActive]         = ImVec4(0.235f, 0.239f, 0.267f, 1.0f);
            St.Colors[ImGuiCol_Header]               = ImVec4(0.961f, 0.647f, 0.141f, 0.16f);
            St.Colors[ImGuiCol_HeaderHovered]        = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
            St.Colors[ImGuiCol_HeaderActive]         = ImVec4(0.961f, 0.647f, 0.141f, 0.24f);
            St.Colors[ImGuiCol_Separator]            = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
            St.Colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.961f, 0.647f, 0.141f, 0.5f);
            St.Colors[ImGuiCol_SeparatorActive]      = ImVec4(0.961f, 0.647f, 0.141f, 0.8f);
            St.Colors[ImGuiCol_ResizeGrip]           = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
            St.Colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.961f, 0.647f, 0.141f, 0.5f);
            St.Colors[ImGuiCol_ResizeGripActive]     = ImVec4(0.961f, 0.647f, 0.141f, 0.8f);
            St.Colors[ImGuiCol_Tab]                  = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
            St.Colors[ImGuiCol_TabHovered]           = ImVec4(0.149f, 0.153f, 0.173f, 1.0f);
            St.Colors[ImGuiCol_TabSelected]          = ImVec4(0.102f, 0.106f, 0.122f, 1.0f);
            St.Colors[ImGuiCol_TabSelectedOverline]  = ImVec4(0.961f, 0.647f, 0.141f, 1.0f);
            St.Colors[ImGuiCol_TabDimmed]            = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
            St.Colors[ImGuiCol_TabDimmedSelected]    = ImVec4(0.102f, 0.106f, 0.122f, 1.0f);
            St.Colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
            St.Colors[ImGuiCol_DockingPreview]       = ImVec4(0.961f, 0.647f, 0.141f, 0.4f);
            St.Colors[ImGuiCol_DockingEmptyBg]       = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
            St.Colors[ImGuiCol_TableHeaderBg]        = ImVec4(0.149f, 0.153f, 0.173f, 1.0f);
            St.Colors[ImGuiCol_TableBorderStrong]    = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
            St.Colors[ImGuiCol_TableBorderLight]     = ImVec4(1.0f, 1.0f, 1.0f, 0.04f);
            St.Colors[ImGuiCol_TableRowBg]           = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
            St.Colors[ImGuiCol_TableRowBgAlt]        = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
            St.Colors[ImGuiCol_TextSelectedBg]       = ImVec4(0.961f, 0.647f, 0.141f, 0.30f);
            St.Colors[ImGuiCol_NavCursor]            = ImVec4(0.961f, 0.647f, 0.141f, 0.8f);
            St.Colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
            St.Colors[ImGuiCol_PlotHistogram]        = ImVec4(0.550f, 0.330f, 0.040f, 1.0f);
        }

        // HydraX's Uno look (ThemeColors.xaml "Default", the brushes in App.xaml): slate panels on a
        // deep blue-grey ground, recessed fields, buttons #3D5465, the blue accent #3B82F6, a focus
        // ring #5AA9E6. Buttons and rows as flat and rounded as ImGui draws them.
        void ApplyHydraX(ImGuiStyle& St)
        {
            const ImVec4 Ground = Rgb(0x1E, 0x2A, 0x30), PanelC = Rgb(0x36, 0x45, 0x4F), Recess = Rgb(0x2A, 0x34, 0x39);
            const ImVec4 HeaderRow = Rgb(0x2E, 0x3E, 0x47), ButtonC = Rgb(0x3D, 0x54, 0x65), Blue = Rgb(0x3B, 0x82, 0xF6);
            const ImVec4 TextC = Rgb(0xE2, 0xE8, 0xF0), MutedC = Rgb(0xA3, 0xBC, 0xCB), Outline = Rgb(0x3A, 0x50, 0x60);
            Colours& C = MutableColours();
            C.Base = Ground; C.Panel = PanelC; C.Raised = ButtonC; C.Overlay = HeaderRow;
            C.Text = TextC; C.Muted = MutedC;
            C.Accent = Blue; C.AccentHover = Rgb(0x5B, 0x9B, 0xF8); C.AccentSoft = Rgb(0x3B, 0x82, 0xF6, 0.28f); C.AccentText = Rgb(0xBF, 0xDB, 0xFE); C.OnAccent = Rgb(0x0F, 0x17, 0x2A);   // light blue on the soft blue fill 5.4:1; dark text on the solid blue 4.7:1
            C.AccentU32 = IM_COL32(0x3B, 0x82, 0xF6, 255);
            const ImVec4 Hover = Rgb(0xFF, 0xFF, 0xFF, 0.10f);
            St.Colors[ImGuiCol_Text]                 = TextC;
            St.Colors[ImGuiCol_TextDisabled]         = MutedC;
            St.Colors[ImGuiCol_WindowBg]             = PanelC;
            St.Colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            St.Colors[ImGuiCol_PopupBg]              = HeaderRow;
            St.Colors[ImGuiCol_MenuBarBg]            = Ground;
            St.Colors[ImGuiCol_Border]               = Outline;
            St.Colors[ImGuiCol_FrameBg]              = Recess;
            St.Colors[ImGuiCol_FrameBgHovered]       = Rgb(0x31, 0x3E, 0x45);
            St.Colors[ImGuiCol_FrameBgActive]        = Rgb(0x38, 0x48, 0x51);
            St.Colors[ImGuiCol_TitleBg]              = Ground;
            St.Colors[ImGuiCol_TitleBgActive]        = Ground;
            St.Colors[ImGuiCol_TitleBgCollapsed]     = Ground;
            St.Colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            St.Colors[ImGuiCol_ScrollbarGrab]        = Rgb(0xFF, 0xFF, 0xFF, 0.16f);
            St.Colors[ImGuiCol_ScrollbarGrabHovered] = Rgb(0xFF, 0xFF, 0xFF, 0.26f);
            St.Colors[ImGuiCol_ScrollbarGrabActive]  = Rgb(0xFF, 0xFF, 0xFF, 0.36f);
            St.Colors[ImGuiCol_CheckMark]            = Blue;
            St.Colors[ImGuiCol_SliderGrab]           = Rgb(0x5A, 0xA9, 0xE6, 0.60f);
            St.Colors[ImGuiCol_SliderGrabActive]     = Rgb(0x5A, 0xA9, 0xE6);
            St.Colors[ImGuiCol_Button]               = ButtonC;
            St.Colors[ImGuiCol_ButtonHovered]        = Rgb(0x4D, 0x65, 0x78);   // the white overlay at 10%
            St.Colors[ImGuiCol_ButtonActive]         = Rgb(0x30, 0x42, 0x50);   // the black overlay at 22%
            St.Colors[ImGuiCol_Header]               = Rgb(0x3B, 0x82, 0xF6, 0.30f);
            St.Colors[ImGuiCol_HeaderHovered]        = Hover;
            St.Colors[ImGuiCol_HeaderActive]         = Rgb(0x3B, 0x82, 0xF6, 0.42f);
            St.Colors[ImGuiCol_Separator]            = Rgb(0x1E, 0x2D, 0x36);
            St.Colors[ImGuiCol_SeparatorHovered]     = Rgb(0x5A, 0xA9, 0xE6, 0.60f);
            St.Colors[ImGuiCol_SeparatorActive]      = Rgb(0x5A, 0xA9, 0xE6, 0.90f);
            St.Colors[ImGuiCol_ResizeGrip]           = Rgb(0xFF, 0xFF, 0xFF, 0.0f);
            St.Colors[ImGuiCol_ResizeGripHovered]    = Rgb(0x5A, 0xA9, 0xE6, 0.60f);
            St.Colors[ImGuiCol_ResizeGripActive]     = Rgb(0x5A, 0xA9, 0xE6, 0.90f);
            St.Colors[ImGuiCol_Tab]                  = Ground;
            St.Colors[ImGuiCol_TabHovered]           = ButtonC;
            St.Colors[ImGuiCol_TabSelected]          = PanelC;
            St.Colors[ImGuiCol_TabSelectedOverline]  = Blue;
            St.Colors[ImGuiCol_TabDimmed]            = Ground;
            St.Colors[ImGuiCol_TabDimmedSelected]    = PanelC;
            St.Colors[ImGuiCol_TabDimmedSelectedOverline] = Rgb(0xFF, 0xFF, 0xFF, 0.0f);
            St.Colors[ImGuiCol_DockingPreview]       = Rgb(0x3B, 0x82, 0xF6, 0.45f);
            St.Colors[ImGuiCol_DockingEmptyBg]       = Ground;
            St.Colors[ImGuiCol_TableHeaderBg]        = HeaderRow;
            St.Colors[ImGuiCol_TableBorderStrong]    = Rgb(0x1E, 0x2D, 0x36);
            St.Colors[ImGuiCol_TableBorderLight]     = Rgb(0x1E, 0x2D, 0x36, 0.6f);
            St.Colors[ImGuiCol_TableRowBg]           = Rgb(0xFF, 0xFF, 0xFF, 0.0f);
            St.Colors[ImGuiCol_TableRowBgAlt]        = Rgb(0x00, 0x00, 0x00, 0.10f);
            St.Colors[ImGuiCol_TextSelectedBg]       = Rgb(0x3B, 0x82, 0xF6, 0.40f);
            St.Colors[ImGuiCol_NavCursor]            = Rgb(0x5A, 0xA9, 0xE6);
            St.Colors[ImGuiCol_ModalWindowDimBg]     = Rgb(0x00, 0x00, 0x00, 0.55f);
            St.Colors[ImGuiCol_PlotHistogram]        = Rgb(0x2A, 0x5F, 0xB8);
        }
    }

    Colours& MutableColours()
    {
        static Colours C;
        return C;
    }

    int Count() { return (int)std::size(Themes); }
    const char* Name(int Index) { return Themes[Index].Name; }
    const char* About(int Index) { return Themes[Index].About; }

    int CurrentIndex()
    {
        if (Chosen() >= 0) return Chosen();
        const std::string Saved = SettingsManager::GetSetting("ui_theme", "graywolf");
        Chosen() = 0;
        for (int i = 0; i < Count(); i++)
            if (Saved == Themes[i].Key) Chosen() = i;
        return Chosen();
    }

    void PinDefault() { Chosen() = 0; }

    void Apply(int Index, bool Remember)
    {
        Chosen() = std::clamp(Index, 0, Count() - 1);
        ImGuiStyle& St = ImGui::GetStyle();
        ImGui::StyleColorsDark();   // the colours not set below, from a known base
        if (Chosen() == 1) ApplyHydraX(St); else ApplyGrayWolf(St);
        if (Remember) SettingsManager::SetSetting("ui_theme", Themes[Chosen()].Key);
    }

    bool ApplyByName(const std::string& Key)
    {
        for (int i = 0; i < Count(); i++)
            if (_stricmp(Key.c_str(), Themes[i].Key) == 0 || _stricmp(Key.c_str(), Themes[i].Name) == 0) { Apply(i, false); return true; }
        return false;
    }

    Fonts& Font()
    {
        static Fonts F;
        return F;
    }

    void Load(float Dpi)
    {
        Scale = Dpi;
        ImGuiIO& io = ImGui::GetIO();
        Fonts& F = Font();
        F.Body = AddFace("C:\\Windows\\Fonts\\segoeui.ttf", L"C:\\Windows\\Fonts\\segoeui.ttf", F.BodySize * Dpi);
        if (!F.Body) F.Body = io.Fonts->AddFontDefault();
        F.Strong = AddFace("C:\\Windows\\Fonts\\seguisb.ttf", L"C:\\Windows\\Fonts\\seguisb.ttf", F.BodySize * Dpi);
        if (!F.Strong) F.Strong = F.Body;
        io.FontDefault = F.Body;
        io.FontGlobalScale = 1.0f;

        ImGui::StyleColorsDark();
        ImGuiStyle& St = ImGui::GetStyle();
        St.FontSizeBase = F.BodySize * Dpi;
        St.WindowRounding = 6.0f; St.ChildRounding = 6.0f; St.FrameRounding = 5.0f; St.PopupRounding = 6.0f;
        St.GrabRounding = 4.0f; St.TabRounding = 5.0f; St.ScrollbarRounding = 6.0f;
        St.WindowPadding = ImVec2(12, 10); St.FramePadding = ImVec2(9, 5); St.ItemSpacing = ImVec2(8, 7); St.CellPadding = ImVec2(7, 4);
        St.WindowBorderSize = 0.0f; St.ChildBorderSize = 0.0f; St.PopupBorderSize = 0.0f; St.FrameBorderSize = 0.0f; St.TabBarBorderSize = 0.0f;
        St.ScrollbarSize = 12.0f; St.GrabMinSize = 6.0f; St.SeparatorTextBorderSize = 1.0f;
        Apply(CurrentIndex(), false);
        St.ScaleAllSizes(Dpi);
    }

    void PushTitle() { ImGui::PushFont(Font().Strong, Font().TitleSize * Scale); }
    void PushSmall() { ImGui::PushFont(nullptr, Font().SmallSize * Scale); }
    void PopFont() { ImGui::PopFont(); }

    void PushPrimary()
    {
        ImGui::PushStyleColor(ImGuiCol_Button, Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentHover);
        ImGui::PushStyleColor(ImGuiCol_Text, OnAccent);
        ImGui::PushFont(Font().Strong, 0.0f);
    }

    void PopPrimary()
    {
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
    }

    bool PrimaryButton(const char* Label, const ImVec2& Size, bool Enabled)
    {
        if (!Enabled)
        {
            ImGui::BeginDisabled();
            const bool Hit = ImGui::Button(Label, Size);
            ImGui::EndDisabled();
            return Hit;
        }
        PushPrimary();
        const bool Hit = ImGui::Button(Label, Size);
        PopPrimary();
        return Hit;
    }

    bool IconButton(const char* Id, const char* Glyph, const char* Tooltip, bool On)
    {
        const float H = ImGui::GetFrameHeight();
        ImGui::PushID(Id);
        ImGui::PushStyleColor(ImGuiCol_Button, On ? AccentSoft : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, On ? ImVec4(Accent.x, Accent.y, Accent.z, 0.24f) : ImVec4(1, 1, 1, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.14f));
        if (On) ImGui::PushStyleColor(ImGuiCol_Text, AccentText);
        const bool Hit = ImGui::Button(Glyph, ImVec2(H, H));
        ImGui::PopStyleColor(On ? 4 : 3);
        if (Tooltip && *Tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", Tooltip);
        ImGui::PopID();
        return Hit;
    }

    bool IconLabelButton(const char* Glyph, const char* Label, const ImVec2& Size)
    {
        char Text[256];
        snprintf(Text, sizeof(Text), "%s  %s", Glyph, Label);
        return ImGui::Button(Text, Size);
    }

    bool Pill(const char* Label, bool On, const ImVec2& Size)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, On ? AccentSoft : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, On ? ImVec4(Accent.x, Accent.y, Accent.z, 0.24f) : ImVec4(1, 1, 1, 0.07f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(Accent.x, Accent.y, Accent.z, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_Text, On ? AccentText : Muted);
        const bool Hit = ImGui::Button(Label, Size);
        ImGui::PopStyleColor(4);
        return Hit;
    }

    bool Section(const char* Label, bool DefaultOpen)
    {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, Muted);
        ImGui::PushFont(Font().Strong, 0.0f);
        const bool Open = ImGui::TreeNodeEx(Label, ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth
                                                   | (DefaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0));
        ImGui::PopFont();
        ImGui::PopStyleColor(2);
        return Open;
    }
}

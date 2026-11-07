#pragma once

// The viewport toolbar's row breaking: the bar is as wide as the 3D view allows and goes onto a second row when its next item would
// not fit, so no control is ever out of reach in a narrow window. Begin once per frame with the width the bar may use, then call
// SameLine in place of ImGui::SameLine before each item or group (ImGui).

#include "imgui.h"

namespace PreviewToolbar
{
    inline float& MaxWidth()
    {
        static float W = 1.0e9f;   // function-local: one value for the bar being drawn (the UI thread)
        return W;
    }

    inline void Begin(float Width) { MaxWidth() = Width; }

    // What an ImGui::Button / Theme::Pill with this label is wide, and an icon button
    inline float ButtonWidth(const char* Label) { return ImGui::CalcTextSize(Label, nullptr, true).x + 2.0f * ImGui::GetStyle().FramePadding.x; }
    inline float IconWidth() { return ImGui::GetFrameHeight(); }

    // The next item (NextWidth wide, Spacing after the last one) on this row when it fits, else it starts a new row.
    // Spacing below 0: ImGui's own.
    inline void SameLine(float Spacing, float NextWidth)
    {
        const float Gap = Spacing < 0.0f ? ImGui::GetStyle().ItemSpacing.x : Spacing;
        const float Right = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        if (Right + Gap + NextWidth <= MaxWidth()) ImGui::SameLine(0.0f, Spacing);
    }
}

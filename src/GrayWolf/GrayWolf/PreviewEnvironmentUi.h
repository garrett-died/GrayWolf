#pragma once

// The viewport's environment and bloom settings (PreviewLighting's environment fields): read from
// graywolf.json, and the toolbar's controls that change and save them.

struct PreviewLighting;
namespace SkyLighting { class Service; }

// The environment, its turn and strength, the backdrop, the bloom: from the settings (preview_environment,
// preview_env_rotation, preview_env_intensity, preview_env_backdrop, preview_env_backdrop_blur,
// preview_bloom, preview_bloom_strength); PreviewLighting's defaults for those not set
void LoadPreviewEnvironmentSettings(PreviewLighting& L);

// The Lighting menu (the BO3 sky states, Fullbright, the environments), the sky chip with the background colour flyout, a bake's
// progress, and a button whose popup holds the environment's turn, strength, the backdrop and the bloom, on the current line
// (ImGui); saved as they are let go. Background: the display colour seen where the sky is off.
void DrawPreviewEnvironmentControls(PreviewLighting& L, SkyLighting::Service& Sky, float (&Background)[3]);
// ui-shot: the Lighting menu opened (or closed) on the next frame it is drawn
void RequestLightingMenu(bool Open);

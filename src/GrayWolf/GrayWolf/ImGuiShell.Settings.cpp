#include "stdafx.h"

// The Settings page: every setting GrayWolf has, in one place, each with a plain explanation under
// its control (what it does, when to change it, the safe choice, any danger). The list is data
// (Entries) so the page can search it and a self-test can check every entry has its words; the
// controls that need more than a switch or a choice (the folder, languages, the games' own
// settings, Blender, updates) are drawn by their owners (the "@..." keys).
#include "ImGuiShell.Internal.h"
#include "AppLog.h"
#include "ExportNaming.h"
#include "GameBadges.h"
#include "FileSystems.h"
#include "ImGuiDialogs.h"
#include "ImGuiBlender.h"
#include "ImGuiLoadGamePanel.h"
#include "ImGuiLoadGamePanel.Common.h"
#include "ImGuiUpdates.h"
#include "SettingsManager.h"
#include "UiAsync.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ShellUi
{
    namespace SettingsPage
    {
        namespace
        {
            Entry Toggle(const char* Section, const char* Group, const char* Key, const char* Label, const char* Default, const char* Help)
            {
                return { Section, Group, Kind::Toggle, Key, Label, Help, Default, {} };
            }
            Entry Choice(const char* Section, const char* Group, const char* Key, const char* Label, const char* Default, std::vector<Option> Options,
                         const char* Help)
            {
                return { Section, Group, Kind::Choice, Key, Label, Help, Default, std::move(Options) };
            }
            Entry PathOf(const char* Section, const char* Group, const char* Key, const char* Label, const char* Help)
            {
                return { Section, Group, Kind::Path, Key, Label, Help, "", {} };
            }
            Entry Custom(const char* Section, const char* Group, const char* Key, const char* Label, const char* Help)
            {
                return { Section, Group, Kind::Custom, Key, Label, Help, "", {} };
            }

            bool ContainsNoCase(const std::string& Text, const std::string& Needle)
            {
                if (Needle.empty()) return true;
                const auto It = std::search(Text.begin(), Text.end(), Needle.begin(), Needle.end(),
                                            [](char A, char B) { return std::tolower((unsigned char)A) == std::tolower((unsigned char)B); });
                return It != Text.end();
            }
        }

        const std::vector<Entry>& Entries()
        {
            static const std::vector<Entry> All = [] {
                const char* App = "Look";
                const char* Exp = "Export";
                const char* Gm = "Games and maps";
                const char* Tools = "Tools";
                const char* Prev = "Preview";
                const char* Prog = "Program";
                std::vector<Entry> E;

                E.push_back(Custom(App, "Theme", "@theme", "Theme",
                    "The colours of the window. GrayWolf is the default (dark, orange accent); HydraX copies the blue-grey look of the HydraX app. "
                    "It only changes colours, so it is safe to switch at any time."));

                E.push_back(Custom(Exp, "Where files go", "@output", "Export folder",
                    "The folder your exports are written to (spaces and quotes around a pasted path are removed). GrayWolf makes one folder per game inside it. Pick a drive with plenty of free "
                    "space: a whole map or a game's images can be several gigabytes. Changing it does not move exports you already made."));
                E.push_back(Custom(Exp, "Where files go", "@languages", "Languages",
                    "Which languages' voices and subtitles are exported. \"The install's\" uses the language your game is set to (read from Steam or "
                    "Battle.net); \"All\" exports every language the game holds, which is much bigger. Things that belong to no language are always "
                    "exported. If GrayWolf cannot find your game's language (no Steam or Battle.net information), it exports every language. "
                    "If you are unsure, leave it on the default."));

                E.push_back(Choice(Exp, "Images", "exportimg", "Image format", "PNG", { { "PNG", "PNG" }, { "TGA", "TGA" }, { "DDS", "DDS" } },
                    "The file type textures are saved as. PNG is the safe choice: nearly every program opens it. TGA suits older tools. DDS keeps "
                    "the game's own compressed texture (smaller and faster to write), but some programs cannot open it."));
                E.push_back(Custom(Exp, "Images", "@imagesfolder", "Where the images go",
                    "Every exported image is written to the \"images\" folder inside the game's export folder, whichever asset used it. "
                    "There is no switch for this. (An old setting named global_images is still kept "
                    "in your settings file; no part of GrayWolf reads it.)"));
                E.push_back(Toggle(Exp, "Images", "patchnormals", "Fix normal maps", "true",
                    "Many games store a normal map with only two channels (red and green). With this on, GrayWolf rewrites the blue channel of "
                    "the normal maps it saves as PNG or TGA, so they look right in other tools. Some games' pictures are fixed while they are "
                    "converted; for Black Ops III, Black Ops 4 and Cold War it is done at the end of the run, once the materials that use each "
                    "picture are known. A DDS file is the game's own data and is never changed. Leave it on unless your tool wants the raw data."));
                E.push_back(Toggle(Exp, "Images", "patchcolor", "Strip the alpha from colour maps (Infinite Warfare)", "true",
                    "Used by the Infinite Warfare reader only: its colour maps carry a hidden alpha channel that makes them look transparent in "
                    "most tools. With this on that channel is removed. Other games ignore it."));

                E.push_back(Choice(Exp, "Sounds", "exportsnd", "Sound format", "OPUS",
                    { { "OPUS", "Opus (small, lossy)" }, { "WAV", "WAV (lossless)" }, { "FLAC", "FLAC (lossless, compressed)" } },
                    "Opus is the default: about a tenth of WAV's size and fine for listening and most uses. WAV keeps the sound as the game stores "
                    "it, the largest files. FLAC is lossless at about half of WAV's size. Pick a lossless one only if you will edit the audio."));
                E.push_back(Toggle(Exp, "Sounds", "keepsndpath", "Keep each sound's folder", "true",
                    "Only used when you export files with the command-line \"file\" mode: on keeps each sound in its game folder under \"sounds\", off puts "
                    "all the sounds in one folder. Normal game exports always keep the folders."));
                E.push_back(Toggle(Exp, "Sounds", "usesabindexbo4", "Name unnamed Black Ops 4 sounds by position", "false",
                    "Black Ops 4 only. Some of its sounds have no known name; they are normally saved as xsound_ followed by a number. With this on "
                    "they are saved under the sound bank's file name and their position in the bank instead. Sounds that have a name are not "
                    "affected."));
                E.push_back(Toggle(Exp, "Sounds", "skipblankaudio", "Skip empty sounds", "false",
                    "In the games that keep sounds in sound banks, some entries hold no audio. With this on they are left out instead of "
                    "written as empty files. Safe to turn on; off by default so nothing is hidden from you."));

                E.push_back(Toggle(Exp, "Model formats", "export_castmdl", "Cast (.cast)", "true",
                    "The default model format: carries the mesh, skeleton and materials. Opens in Blender and Maya with the free Cast plugins "
                    "(github.com/dtzxporter/cast). Keep this on unless you need another format."));
                E.push_back(Toggle(Exp, "Model formats", "export_semodel", "SEModel (.semodel)", "false",
                    "An older model format read by the SE Tools plugins for Blender and Maya. Turn it on if your tools expect it."));
                E.push_back(Toggle(Exp, "Model formats", "export_gltf", "glTF (.gltf)", "false",
                    "A modern open model format, written as text plus separate files. Opens in Blender, many viewers and game engines."));
                E.push_back(Toggle(Exp, "Model formats", "export_glb", "GLB (.glb)", "false",
                    "Like glTF, but the model's data is packed into one file. The textures are still separate image files next to it, so keep them together."));
                E.push_back(Toggle(Exp, "Model formats", "export_obj", "OBJ (.obj)", "false",
                    "A very old, very widely read format. Geometry and basic materials only: no skeleton."));
                E.push_back(Toggle(Exp, "Model formats", "export_ma", "Maya ASCII (.ma)", "false",
                    "A scene file Maya opens directly, with the joints. To attach the mesh to the joints, run the extra _BIND.mel file written next to it in Maya. "
                    "Handy if you work in Maya without the Cast plugin."));
                E.push_back(Toggle(Exp, "Model formats", "export_smd", "SMD (.smd)", "false",
                    "The Source engine's model format, read by many older tools. Always written in the game's own units."));
                E.push_back(Toggle(Exp, "Model formats", "export_xna", "XNALara (.mesh.ascii)", "false",
                    "A text format for XNALara and similar posing programs. Turn it on only if you use one of them."));
                E.push_back(Toggle(Exp, "Model formats", "export_xmexport", "XMODEL_EXPORT (Mod Tools text)", "false",
                    "The text model format of the Call of Duty Mod Tools. Always written in the game's own units. Only useful if you are going to "
                    "compile the model with the Mod Tools."));
                E.push_back(Toggle(Exp, "Model formats", "export_xmbin", "XMODEL_BIN (Mod Tools binary)", "false",
                    "The binary model format of the Call of Duty Mod Tools. Always written in the game's own units."));
                E.push_back(Choice(Exp, "Model formats", "unit", "Model size unit", "Centimeter",
                    { { "Centimeter", "Centimeter" }, { "Inch", "Inch (the game's own units)" } },
                    "The games work in inches. Centimeter scales models so they come out at a normal size in Blender, Maya and similar tools. Inch "
                    "keeps the game's numbers. glTF/GLB are always in metres, and the Mod Tools formats and SMD are always in inches."));
                E.push_back(Toggle(Exp, "Model formats", "exportalllods", "Export every level of detail", "false",
                    "Models normally export only their best-quality version. With this on, the lower-detail versions are written as well: many "
                    "more files. Leave off unless you need them."));
                E.push_back(Toggle(Exp, "Model formats", "exportvtxcolor", "Write vertex colours", "false",
                    "Some games paint colour into the model's vertices (used to tint or blend materials). On writes it into the model; off leaves "
                    "it out. A new settings file starts with it off. (If the key were missing altogether the readers would treat it as on, but "
                    "GrayWolf adds it as off at every start, so that does not happen.)"));

                E.push_back(Toggle(Exp, "Animation formats", "export_castanim", "Cast animation (.cast)", "true",
                    "The default animation format, read by the Cast plugins for Blender and Maya. Includes the notetracks (the markers a game "
                    "puts on an animation)."));
                E.push_back(Toggle(Exp, "Animation formats", "export_seanim", "SEAnim (.seanim)", "false",
                    "The older animation format of the SE Tools plugins. Turn it on if your tools expect it."));
                E.push_back(Toggle(Exp, "Animation formats", "export_directxanim", "XANIM_EXPORT (Mod Tools text)", "false",
                    "The text animation format of the Call of Duty Mod Tools. Only useful if you will compile the animation with them."));
                E.push_back(Choice(Exp, "Animation formats", "directxanim_ver", "XANIM_EXPORT version", "17",
                    { { "17", "World at War (version 17)" }, { "BlackOps", "Black Ops" } },
                    "Which flavour of XANIM_EXPORT is written. Only matters when XANIM_EXPORT is on: pick the one your Mod Tools version reads."));

                E.push_back(Custom(Gm, "A game's own choices", "@gamesettings", "Settings of a game",
                    "Choices only some games or kinds of asset have: how maps are written, extra Black Ops III Mod Tools files, whether scripts are "
                    "decompiled. Pick the game; each choice says what it does."));

                E.push_back(Custom(Tools, "Blender", "@blender", "blender.exe",
                    "The Blender program GrayWolf starts for \"Open in Blender\" on an exported map. Leave it empty and GrayWolf finds Blender itself "
                    "(environment variable, Program Files, Steam, the registry, PATH). Only set it if you have several Blender versions or a portable one."));
                E.push_back(PathOf(Tools, "Helper programs", "ffmpeg_exe", "ffmpeg.exe",
                    "Needed only to read sounds stored as WMA (some older games). Leave it empty and GrayWolf looks for it; if those sounds fail, "
                    "download ffmpeg and point to ffmpeg.exe here."));
                E.push_back(PathOf(Tools, "Helper programs", "acts_exe", "acts.exe",
                    "A helper that reads Black Ops III's raw files and scripts, and decompiles compiled scripts. Leave empty to look for it on your "
                    "PATH; set it if script export says it cannot find it."));
                E.push_back(PathOf(Tools, "Helper programs", "gsc_tool_exe", "gsc-tool.exe",
                    "A helper that turns compiled scripts (GSC) back into text, used when a game's scripts are exported as Decompiled. Leave empty "
                    "to look for it on your PATH."));
                E.push_back(PathOf(Tools, "Helper programs", "cod_luajit_decompiler_exe", "LuaJIT decompiler",
                    "A helper that turns compiled LuaJIT files back into readable Lua, used when scripts are exported as Decompiled. Leave empty "
                    "to look for it on your PATH."));
                E.push_back(PathOf(Tools, "Helper programs", "cod_lua_decompiler_exe", "Lua decompiler",
                    "The same for compiled plain Lua files. Leave empty to look for it on your PATH."));

                E.push_back(Custom(Prev, "Screenshots", "@screenshots", "Screenshots folder",
                    "Where the camera button on the 3D view (or Ctrl+Shift+S) saves pictures. Left empty, GrayWolf uses the folder the screenshots "
                    "were last saved to, and Pictures\\GrayWolf when there is none yet. Spaces and quotes around a pasted path are removed."));
                E.push_back(Choice(Prev, "Screenshots", "preview_shot_scale", "Screenshot size", "1", { { "1", "1x" }, { "2", "2x" }, { "4", "4x" } },
                    "How many times larger than the 3D view the picture is. Pictures have a transparent background. Bigger is sharper and slower."));
                E.push_back(Custom(Prev, "The 3D view", "@previewinfo", "Light, shadows and quality",
                    "These are set with the buttons on the toolbar over the model (exposure, lights, environment, shadows, ground, quality), and "
                    "remembered. They are not repeated here so there is one place for each."));

                E.push_back(Custom(Prog, "Updates", "@updates", "Updates",
                    "At startup GrayWolf installs newer name lists (so assets have readable names) and tells you when a newer GrayWolf exists; it "
                    "never replaces the program without you pressing Download and Restart. The GitHub token is only for a private repository: it "
                    "is read from the GitHub CLI at each check, sent to GitHub only and never saved."));
                E.push_back(Custom(Prog, "Logs and files", "@logs", "Session log",
                    "GrayWolf writes what it does to a log file beside the program. Open it when you report a problem. The detailed log times "
                    "every step, so it grows faster: turn it on only when asked. It is not remembered: it is off again at every start. Your settings "
                    "are saved in graywolf.json next to the program."));
                return E;
            }();
            return All;
        }

        bool Matches(const Entry& E, const std::string& Query)
        {
            return ContainsNoCase(E.Label, Query) || ContainsNoCase(E.Help, Query) || ContainsNoCase(E.Group, Query) ||
                   ContainsNoCase(E.Section, Query) || ContainsNoCase(E.Key, Query);
        }

        const char* DiskSettingHelp(const std::string& Key)
        {
            struct H { const char* Key; const char* Text; };
            static const H All[] = {
                { "map_png", "Map textures use whatever Image format you chose above. Choose \"+ PNG\" to also write PNG copies of the colour maps for glTF "
                             "tools other than Blender. It does nothing if the Image format is already PNG. It only adds files." },
                { "map_textures", "DDS map textures larger than this are also saved at the chosen size (with mipmaps) and used from there. 2K or 1K makes "
                                  "the map's textures smaller; Full keeps every texture as it is. Textures exported as PNG or TGA are not resized." },
                { "map_cast", "Besides the .glb that Blender imports, also write the map as a Cast scene for Maya and the Cast plugins. Safe; it only "
                              "adds files." },
                { "map_radiant", "Also write the map as a Radiant .map for the Black Ops III Mod Tools: its collision brushes, entities and model "
                                 "placements. Experimental: that these load in Radiant is not yet confirmed. It only adds files." },
                { "material_gdt", "Also write Black Ops III Mod Tools material entries (GDT) for the exported materials, so the Mod Tools can use "
                                  "them. Only useful if you are going to use the Mod Tools. Experimental. It only adds files." },
                { "sound_aliases", "Also write the zone's sound aliases as a Black Ops III Mod Tools alias CSV beside the sounds. Only useful for "
                                   "the Mod Tools. It only adds a file." },
                { "map_collision", "Also write the map's collision (the invisible walls and floors the game uses) as a JSON file. For tools that "
                                   "need the collision; most people do not." },
                { "map_content", "Everything exports the whole map. Terrain only writes just the ground meshes and their materials: quick to "
                                 "export and open when you only want to look at the terrain." },
                { "decompile", "Compiled scripts and Lua files are saved as bytecode by default, which you cannot read. Decompiled turns them back "
                               "into text using the helper programs (see Tools); it takes longer and the result is a reconstruction, not the "
                               "original source. If a helper program cannot be found, the scripts are saved as bytecode instead." },
                { "ui_res", "Black Ops 4 keeps its interface images twice, at 4K and at 1080p. Pick which are exported; \"both\" doubles the "
                            "space they take." },
                { "language", "Call of Duty 1 and 2 shipped in several languages; an install holds English plus at most one more. \"As the export "
                              "languages\" follows the Languages setting; or pick one language here." },
            };
            for (const H& Each : All)
                if (Key == Each.Key) return Each.Text;
            return nullptr;
        }

        bool SelfTest(std::string& Report)
        {
            bool Ok = true;
            const auto Fail = [&](const std::string& What) { Ok = false; Report += "FAIL " + What + "\n"; };
            std::set<std::string> Keys;
            for (const Entry& E : Entries())
            {
                if (!Keys.insert(E.Key).second) Fail(std::string("duplicate key ") + E.Key);
                if (std::string_view(E.Label).empty() || std::string_view(E.Group).empty() || std::string_view(E.Section).empty()) Fail(std::string("empty label, group or section for ") + E.Key);
                if (std::strlen(E.Help) < 60) Fail(std::string("explanation too short for ") + E.Key);
                if (E.K == Kind::Choice)
                {
                    bool Has = false;
                    for (const Option& O : E.Options) Has = Has || _stricmp(O.Value, E.Default) == 0;
                    if (!Has) Fail(std::string("default is not an option for ") + E.Key);
                }
                if (E.K != Kind::Custom && std::string_view(E.Key).starts_with('@')) Fail(std::string("a custom key on a plain control: ") + E.Key);
            }
            // every setting of every game has its words
            size_t Games = 0, PerGame = 0;
            for (const DiskGame::IDiskGame* G : DiskGame::Games())
            {
                Games++;
                for (const DiskGame::Setting* S : G->AllSettings())
                {
                    PerGame++;
                    if (DiskSettingHelp(S->Key) == nullptr) Fail(std::string("no explanation for the setting ") + S->Key + " of " + G->Id());
                }
            }
            // the search
            const Entry& First = Entries().front();
            if (!Matches(First, "THEME")) Fail("search is not case-insensitive");
            if (Matches(First, "zzzz-no-such-word")) Fail("search matches everything");
            char Line[160];
            std::snprintf(Line, sizeof(Line), "entries=%zu games=%zu game_settings=%zu\n", Entries().size(), Games, PerGame);
            Report += Line;
            return Ok;
        }

        namespace
        {
            // the explanation, under its control: smaller, muted, wrapped to the page
            void HelpLine(const char* Text, float Width)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Width);
                ImGui::TextUnformatted(Text);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            // The settings a list, an index or an export reads while it runs: the Export section (folder,
            // languages, formats), the games' choices and the helper programs. The page locks them
            // while a worker runs; the look, Blender, screenshots, updates and logs are not read.
            bool ReadByRun(const Entry& E)
            {
                const std::string_view Section = E.Section, Group = E.Group;
                return Section == "Export" || Section == "Games and maps" || (Section == "Tools" && Group == "Helper programs");
            }

            std::string ValueOf(const Entry& E) { return SettingsManager::GetSetting(E.Key, E.Default); }

            void DrawChoice(const Entry& E)
            {
                const std::string Value = ValueOf(E);
                const char* Shown = Value.c_str();
                for (const Option& O : E.Options)
                    if (_stricmp(O.Value, Value.c_str()) == 0) Shown = O.Label;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(E.Label);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 15.0f);
                if (ImGui::BeginCombo((std::string("##") + E.Key).c_str(), Shown))
                {
                    for (const Option& O : E.Options)
                        if (ImGui::Selectable(O.Label, _stricmp(O.Value, Value.c_str()) == 0)) SettingsManager::SetSetting(E.Key, O.Value);
                    ImGui::EndCombo();
                }
            }

            void DrawPath(const Entry& E)
            {
                static std::map<std::string, std::array<char, 1024>> Buffers;
                auto& Buf = Buffers[E.Key];
                const std::string Id = std::string("settings.") + E.Key;
                if (std::string Picked; UiAsync::Picked(Id, Picked)) SettingsManager::SetSetting(E.Key, Picked);
                if (!ImGui::IsAnyItemActive()) SetBuf(Buf.data(), Buf.size(), SettingsManager::GetSetting(E.Key, ""));
                ImGui::TextUnformatted(E.Label);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30.0f);
                ImGui::InputTextWithHint((std::string("##") + E.Key).c_str(), "Looked for by itself", Buf.data(), Buf.size());
                if (ImGui::IsItemDeactivatedAfterEdit()) SettingsManager::SetSetting(E.Key, TrimPath(Buf.data()));
                ImGui::SameLine();
                if (ImGui::Button((std::string("Browse\xE2\x80\xA6##") + E.Key).c_str()))
                    UiAsync::PickOpenFile(Id, std::string("Select ") + E.Label, { { L"Program", L"*.exe" } });
                ImGui::SameLine();
                ImGui::BeginDisabled(Buf[0] == 0);
                if (ImGui::Button((std::string("Clear##") + E.Key).c_str())) SettingsManager::SetSetting(E.Key, "");
                ImGui::EndDisabled();
            }

            void DrawGameSettings(LoadGamePanel& LoadGame, float Width)
            {
                const auto& Games = DiskGame::Games();
                if (Games.empty()) return;
                static int Picked = -1;
                static std::string Followed;   // the id of the game the page follows (the one read in Extract)
                if (const DiskGame::IDiskGame* Loaded = LoadGame.LoadedGame(); (Loaded ? std::string(Loaded->Id()) : std::string()) != Followed)
                {
                    Followed = Loaded ? Loaded->Id() : "";
                    for (size_t i = 0; i < Games.size(); i++)
                        if (Games[i] == Loaded) Picked = (int)i;
                }
                if (Picked < 0 || Picked >= (int)Games.size()) Picked = 0;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Game");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
                if (ImGui::BeginCombo("##settingsgame", Games[(size_t)Picked]->Title()))
                {
                    for (size_t i = 0; i < Games.size(); i++)
                        if (ImGui::Selectable(Games[i]->Title(), (int)i == Picked)) Picked = (int)i;
                    ImGui::EndCombo();
                }
                const DiskGame::IDiskGame& G = *Games[(size_t)Picked];
                const auto Settings = G.AllSettings();
                if (Settings.empty()) ImGui::TextDisabled("This game has no choices of its own.");
                ImGui::Spacing();
                for (const DiskGame::Setting* S : Settings)
                {
                    const std::string Key = LoadGameUi::SettingKey(G, *S);
                    const std::string Value = SettingsManager::GetSetting(Key, S->Default);
                    std::string Preview = Value;
                    for (const auto& [V, L] : S->Values)
                        if (V == Value) Preview = L;
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(S->Label);
                    ImGui::SameLine(ImGui::GetFontSize() * 14.0f);
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 15.0f);
                    if (ImGui::BeginCombo(("##gs_" + Key).c_str(), Preview.c_str()))
                    {
                        for (const auto& [V, L] : S->Values)
                            if (ImGui::Selectable(L.c_str(), V == Value)) SettingsManager::SetSetting(Key, V);
                        ImGui::EndCombo();
                    }
                    std::string Used;
                    for (DiskGame::Kind K : S->Concerns) Used += (Used.empty() ? "" : ", ") + std::string(DiskGame::KindName(K));
                    const char* Help = DiskSettingHelp(S->Key);
                    const std::string Text = std::string(Help ? Help : "") + "  (Used when exporting: " + Used + ".)";
                    HelpLine(Text.c_str(), Width);
                }
            }

            void DrawCustom(const Entry& E, ShellState& S, LoadGamePanel& LoadGame, float Width)
            {
                (void)S;
                const std::string K = E.Key;
                if (K == "@theme")
                {
                    for (int i = 0; i < Theme::Count(); i++)
                    {
                        if (i > 0) ImGui::SameLine();
                        if (Theme::Pill(Theme::Name(i), Theme::CurrentIndex() == i)) Theme::Apply(i, true);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Theme::About(i));
                    }
                    ImGui::Spacing();
                }
                else if (K == "@output") LoadGame.DrawOutputFolder();
                else if (K == "@languages") LoadGame.DrawLanguages();
                else if (K == "@imagesfolder")
                {
                    // the folder the images of the game read now go to; the game's id stands in until one is read
                    const DiskGame::IDiskGame* Loaded = LoadGame.LoadedGame();
                    const std::filesystem::path Images = std::filesystem::path(AbsoluteFolder(SettingsManager::GetSetting("disk_out", "exported_files"))) /
                                                         (Loaded ? std::string(Loaded->Id()) : std::string("<game>")) / "images";
                    ImGui::TextDisabled("%s", Images.string().c_str());
                }
                else if (K == "@gamesettings") DrawGameSettings(LoadGame, Width);
                else if (K == "@blender") DrawBlenderSettings();
                else if (K == "@screenshots") DrawScreenshotSettings();
                else if (K == "@updates") DrawUpdatesSettings();
                else if (K == "@logs")
                {
                    bool Verbose = AppLog::Verbose();
                    if (ImGui::Checkbox("Detailed log (every step timed)", &Verbose)) AppLog::SetVerbose(Verbose);
                    if (ImGui::Button("Open the session log")) UiAsync::OpenInShell(AppLog::SessionPath());
                    ImGui::SameLine();
                    if (ImGui::Button("Show the settings file")) UiAsync::OpenInShell(FileSystems::GetApplicationPath() + "\\graywolf.json");
                    ImGui::Spacing();
                }
            }
        }

        void Draw(ShellState& S, LoadGamePanel& LoadGame)
        {
            const ImGuiViewport* Vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(Vp->WorkPos);
            ImGui::SetNextWindowSize(Vp->WorkSize);
            if (S.SettingsJustOpened) ImGui::SetNextWindowFocus();
            const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoScrollbar;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 1.5f, ImGui::GetFontSize()));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Base);
            if (!ImGui::Begin("Settings##page", nullptr, Flags))
            {
                ImGui::End();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                return;
            }
            const float Em = ImGui::GetFontSize();
            // the header: Back, the title, the search
            if (ImGui::Button("\xE2\x86\x90  Back") || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)))
                S.SettingsOpen = false;
            ImGui::SameLine();
            Theme::PushTitle();
            ImGui::TextUnformatted("Settings");
            Theme::PopFont();
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + Em, ImGui::GetWindowWidth() - Em * 22.0f - ImGui::GetStyle().WindowPadding.x));
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(Em * 20.0f);
            ImGui::InputTextWithHint("##settingssearch", "Search the settings\xE2\x80\xA6  (Ctrl+F)", S.SettingsQuery, sizeof(S.SettingsQuery),
                                     ImGuiInputTextFlags_EscapeClearsAll);
            ImGui::Separator();

            const std::string Query = S.SettingsQuery;
            const bool Locked = LoadGame.WorkerRuns();
            if (Locked)
                ImGui::TextColored(Theme::AccentText, "An export or a load is running. The export settings, the games' choices and the helper "
                                                "programs are locked until it ends, so they cannot change a run under way.");
            // a column of readable width, centred; its scrollbar at its own edge
            const float Width = std::min(ImGui::GetContentRegionAvail().x - Em, Em * 52.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - Width) * 0.5f));
            ImGui::BeginChild("##settingsbody", ImVec2(Width + ImGui::GetStyle().ScrollbarSize, 0), ImGuiChildFlags_None);
            ImGui::PushStyleColor(ImGuiCol_Header, Theme::Raised);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::Overlay);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, Theme::Overlay);

            const auto& All = Entries();
            std::string LastSection, LastGroup;
            bool GroupOpen = false, AnyShown = false;
            for (size_t i = 0; i < All.size(); i++)
            {
                const Entry& E = All[i];
                if (!Matches(E, Query)) continue;
                AnyShown = true;
                if (LastSection != E.Section)
                {
                    if (!LastSection.empty()) ImGui::Spacing();
                    Theme::PushTitle();
                    ImGui::TextUnformatted(E.Section);
                    Theme::PopFont();
                    LastSection = E.Section;
                    LastGroup.clear();
                }
                if (LastGroup != E.Group)
                {
                    LastGroup = E.Group;
                    const bool Jump = !S.SettingsJump.empty() && S.SettingsJump == E.Group;
                    if (!Query.empty() || Jump) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                    const std::string Name = E.Group;
                    const bool Starts = Name == "Theme" || Name == "Where files go" || Name == "Updates";   // open at first, the rest folded
                    GroupOpen = ImGui::CollapsingHeader(E.Group, Starts ? ImGuiTreeNodeFlags_DefaultOpen : 0);
                    if (Jump) { ImGui::SetScrollHereY(0.0f); S.SettingsJump.clear(); }
                }
                if (!GroupOpen) continue;
                ImGui::PushID((int)i);
                const bool Lock = Locked && ReadByRun(E);
                if (Lock) ImGui::BeginDisabled();
                switch (E.K)
                {
                case Kind::Toggle:
                {
                    bool On = ValueOf(E) == "true";
                    if (ImGui::Checkbox(E.Label, &On)) SettingsManager::SetSetting(E.Key, On ? "true" : "false");
                    break;
                }
                case Kind::Choice: DrawChoice(E); break;
                case Kind::Path: DrawPath(E); break;
                case Kind::Custom:
                    if (std::string(E.Key) != "@languages")   // the languages control carries its own caption
                    {
                        ImGui::PushFont(Theme::Font().Strong, 0.0f);
                        ImGui::TextUnformatted(E.Label);
                        ImGui::PopFont();
                    }
                    DrawCustom(E, S, LoadGame, Width);
                    break;
                }
                if (Lock) ImGui::EndDisabled();
                // the plain explanation, under the control (the games' own settings carry theirs as well)
                HelpLine(E.Help, Width);
                ImGui::PopID();
            }
            ImGui::PopStyleColor(3);
            if (!AnyShown) ImGui::TextDisabled("No setting matches \"%s\".", Query.c_str());
            ImGui::Dummy(ImVec2(0, Em * 2.0f));
            ImGui::EndChild();
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            S.SettingsJustOpened = false;
        }
    }
}

bool UiSelfTest(std::string& Report)
{
    const bool Settings = ShellUi::SettingsPage::SelfTest(Report);
    const bool Search = ShellUi::Search::SelfTest(Report);
    const bool Windows = ShellUi::WindowSelfTest(Report) && GameBadges::SelfTest(Report);
    // the Filters panel's types and the search box's type: terms are one state
    bool Types = true;
    {
        ShellUi::AssetsTab A;
        std::snprintf(A.Filter, sizeof(A.Filter), "%s", "crate type:image !type:fx");
        ShellUi::SetTypeMask(A, 0x03);   // models and images ticked in the panel
        const std::string Wrote = A.Filter;
        if (ShellUi::TypeMaskOf(A.Filter) != 0x03 || Wrote.find("crate") != 0 || Wrote.find("!type") != std::string::npos) { Types = false; Report += "FAIL type terms rewritten from the panel\n"; }
        std::snprintf(A.Filter, sizeof(A.Filter), "%s", "type:anim");
        if (ShellUi::TypeMaskOf(A.Filter) != 0x08) { Types = false; Report += "FAIL a typed search ticks its type\n"; }
        // one cutting for the box and the panel: a quote inside a word is not a quoted term
        std::snprintf(A.Filter, sizeof(A.Filter), "%s", "a\"b type:image");
        if (ShellUi::TypeMaskOf(A.Filter) != 0x02) { Types = false; Report += "FAIL a quote inside a word hides the type term\n"; }
        ShellUi::SetTypeMask(A, 0x01);
        if (std::string(A.Filter) != "a\"b type:\"model\"") { Types = false; Report += std::string("FAIL the word with a quote is kept: ") + A.Filter + "\n"; }
        // a type that does not exist is not "all types"
        std::snprintf(A.Filter, sizeof(A.Filter), "%s", "type:foo");
        {
            const ShellUi::TypeSelection Sel = ShellUi::TypeSelectionOf(A.Filter);
            if (!Sel.Named || !Sel.Impossible || Sel.Mask != 0 || Sel.Why.find("type:foo is not a type") != 0) { Types = false; Report += "FAIL an unknown type says why\n"; }
        }
        std::snprintf(A.Filter, sizeof(A.Filter), "%s", "type:image !type:image");
        {
            const ShellUi::TypeSelection Sel = ShellUi::TypeSelectionOf(A.Filter);
            if (!Sel.Impossible || Sel.Why.find("exclude") == std::string::npos) { Types = false; Report += "FAIL excluded types say why\n"; }
            ShellUi::SetTypeMask(A, 0);
            if (A.Filter[0] != 0 || ShellUi::TypeSelectionOf(A.Filter).Named) { Types = false; Report += "FAIL the type terms can be cleared\n"; }
        }
        std::snprintf(A.Filter, sizeof(A.Filter), "%s", "!type:foo");
        if (ShellUi::TypeSelectionOf(A.Filter).Impossible || ShellUi::TypeMaskOf(A.Filter) != 0) { Types = false; Report += "FAIL excluding a type that does not exist excludes nothing\n"; }
        // the box is never cut: text that would not fit is left as it was
        {
            std::string Long(sizeof(A.Filter) - 20, 'w');
            std::snprintf(A.Filter, sizeof(A.Filter), "%s", Long.c_str());
            const bool Fit = ShellUi::SetTypeMask(A, 0x7F);
            if (Fit || std::string(A.Filter) != Long) { Types = false; Report += "FAIL type terms that do not fit are cut into the box\n"; }
            if (!ShellUi::SetTypeMask(A, 0) || std::string(A.Filter) != Long) { Types = false; Report += "FAIL clearing leaves the other terms\n"; }
        }
        std::snprintf(A.Filter, sizeof(A.Filter), "%s", "type:anim");
        ShellUi::SetTypeMask(A, 0);
        if (ShellUi::TypeMaskOf(A.Filter) != 0 || A.Filter[0] != 0) { Types = false; Report += "FAIL All types clears the terms\n"; }
    }
    const bool Text = ShellUi::TextViewer::SelfTest(Report);
    return Settings && Search && Windows && Text && Types;
}

#include "stdafx.h"

#include "SurfaceTypes.h"
#include "CoDAssets.h"
#include "ExportNaming.h"
#include "ZoneTableWalker.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

namespace SurfaceTypes
{
namespace
{
    bool ParseWord(const std::string& Text, uint32_t& Out)
    {
        if (Text.empty()) return false;
        try
        {
            size_t Used = 0;
            const unsigned long long V = std::stoull(Text, &Used, 0);
            if (Used != Text.size() || V > 0xFFFFFFFFull) return false;
            Out = static_cast<uint32_t>(V);
            return true;
        }
        catch (const std::exception&)   // NOLINT(bugprone-empty-catch): stoull throws on text that is no number; that row is skipped
        {
            return false;
        }
    }

    std::string Slurp(const std::filesystem::path& File)
    {
        std::ifstream In(File, std::ios::binary);
        std::stringstream Text;
        Text << In.rdbuf();
        return Text.str();
    }
}

bool Names::Parse(std::string_view Text)
{
    *this = Names();
    std::istringstream In{ std::string(Text) };
    std::string Line;
    while (std::getline(In, Line))
    {
        if (!Line.empty() && Line.back() == '\r') Line.pop_back();
        if (Line.empty() || Line[0] == '#') continue;
        std::istringstream Fields(Line);
        std::string Kind, Name, Value;
        if (!std::getline(Fields, Kind, '|') || !std::getline(Fields, Name, '|') || !std::getline(Fields, Value, '|')) continue;
        uint32_t Word = 0;
        if (!ParseWord(Value, Word)) continue;
        if (Kind == "surface_mask") TypeMask = Word;
        else if (Kind == "game" && !Name.empty()) Games.push_back(Name);
        else if (Kind == "surface" && !Name.empty()) Types.emplace_back(Word, Name);
        else if (Kind == "render" && Name == "opaque") OpaqueBit = Word;
        else if (Kind == "render" && Name == "decal") DecalBit = Word;
    }
    return TypeMask != 0 && !Types.empty();
}

bool Names::HasGame(const std::string& Game) const
{
    return std::find(Games.begin(), Games.end(), Game) != Games.end();
}

Names::State Names::Decode(uint32_t SurfaceFlags, std::string& Name) const
{
    const uint32_t Type = SurfaceFlags & TypeMask;
    if (Type == 0) return State::NoSurface;
    for (const auto& [Value, Declared] : Types)
        if (Value == Type)
        {
            Name = Declared;
            return State::Named;
        }
    return State::Unnamed;
}

bool Names::IsDecal(uint32_t RenderFlags) const
{
    return DecalBit != 0 && (RenderFlags & DecalBit) != 0 && (RenderFlags & OpaqueBit) == 0;
}

const Names* For(const std::string& Profile, bool* ProfileFound)
{
    static std::mutex Lock;
    static std::map<std::string, std::unique_ptr<Names>> Tables;
    const std::lock_guard<std::mutex> Guard(Lock);
    if (ProfileFound != nullptr) *ProfileFound = true;
    auto It = Tables.find(Profile);
    if (It == Tables.end())
    {
        std::unique_ptr<Names> Loaded;
        ZoneTables::Profile P;
        std::string Error;
        if (!ZoneTables::LoadProfile(Profile, P, Error))
        {
            // zonekit/profiles was not found: nothing is cached, so a later call (the folder appearing) looks again
            if (ProfileFound != nullptr) *ProfileFound = false;
            return nullptr;
        }
        auto Table = std::make_unique<Names>();
        if (Table->Parse(Slurp(std::filesystem::path(P.Dir) / "flag_names.txt"))) Loaded = std::move(Table);
        It = Tables.emplace(Profile, std::move(Loaded)).first;
    }
    return It->second.get();
}

void Apply(const std::string& Profile, const Materials::Source& Source, Materials::Description& D)
{
    D.SurfaceType.clear();
    bool Found = true;
    const Names* Table = For(Profile, &Found);
    if (Table == nullptr)
    {
        D.SurfaceWhy = Found ? "no_table" : "table_missing";
        return;
    }
    if (Source.SurfaceFlags < 0)
    {
        D.SurfaceWhy = "not_read";
        return;
    }
    std::string Name;
    switch (Table->Decode(static_cast<uint32_t>(Source.SurfaceFlags), Name))
    {
    case Names::State::Named:
        D.SurfaceType = Name;
        D.SurfaceWhy.clear();
        break;
    case Names::State::NoSurface:
        D.SurfaceWhy = "none_in_source";
        break;
    case Names::State::Unnamed:
        D.SurfaceWhy = std::format("unnamed:0x{:X}", static_cast<uint32_t>(Source.SurfaceFlags) & Table->Mask());
        break;
    }
}

RenderClass ClassOf(const std::string& Profile, int64_t RenderFlags)
{
    if (RenderFlags <= 0) return RenderClass::Unread;
    const Names* Table = For(Profile);
    if (Table == nullptr || !Table->HasRenderBits()) return RenderClass::Unread;
    return Table->IsDecal(static_cast<uint32_t>(RenderFlags)) ? RenderClass::Decal : RenderClass::NotDecal;
}

bool IsDecal(const std::string& Profile, int64_t RenderFlags)
{
    return ClassOf(Profile, RenderFlags) == RenderClass::Decal;
}

bool ReadAndNamesNoSurface(const std::string& Game, int64_t Flags)
{
    if (Flags < 0) return false;
    const std::string Id = ExportNaming::Lower(Game);
    for (const char* Profile : { "bo4", "t7", "t9" })
        if (const Names* Table = For(Profile); Table != nullptr && Table->HasGame(Id))
        {
            std::string Name;
            return Table->Decode(static_cast<uint32_t>(Flags), Name) == Names::State::NoSurface;
        }
    return false;
}

int64_t ReadWord(uint64_t Address)
{
    if (CoDAssets::GameInstance == nullptr) return -1;
    uint8_t Bytes[4] = {};
    if (CoDAssets::GameInstance->Read(Bytes, Address, sizeof(Bytes)) != sizeof(Bytes)) return -1;
    return static_cast<int64_t>(static_cast<uint32_t>(Bytes[0]) | (static_cast<uint32_t>(Bytes[1]) << 8) | (static_cast<uint32_t>(Bytes[2]) << 16) | (static_cast<uint32_t>(Bytes[3]) << 24));
}

std::string ToBo3(const std::string& Name, const std::vector<std::string>& Options)
{
    const std::string Wanted = ExportNaming::Lower(Name);
    for (const std::string& Option : Options)
        if (ExportNaming::Lower(Option) == Wanted) return Option;
    return std::string();
}

int SelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
        if (Ok) return;
        Failed++;
        Failures += std::string("SurfaceTypes: ") + What + (Got.empty() ? "" : " (" + Got + ")") + "\n";
    };
    Names N;
    const bool Parsed = N.Parse("# c\nsurface_mask||0x03F00000\nsurface|metal|0xD00000\nsurface|glass|0x900000\nsurface|wood|0x1500000\nrender|opaque|0x4\nrender|decal|0x8\nbad|row\n");
    Check(Parsed && N.Count() == 3 && N.Mask() == 0x03F00000u, "the rows are read", std::to_string(N.Count()));
    std::string Name;
    // the type is the masked field: the low flag bits and the bits above the mask do not change it (HydraX measured 0xD440A0 as metal)
    Check(N.Decode(0xD440A0, Name) == Names::State::Named && Name == "metal", "0xD440A0 is metal", Name);
    Check(N.Decode(0x940080, Name) == Names::State::Named && Name == "glass", "0x940080 is glass", Name);
    Check(N.Decode(0x1500000 | 0x38000000, Name) == Names::State::Named && Name == "wood", "traversal bits above the mask do not change the type", Name);
    Check(N.Decode(0x000440A0, Name) == Names::State::NoSurface, "a word with no type bits states no surface");
    Check(N.Decode(0x2800000, Name) == Names::State::Unnamed, "0x2800000 names no declared type");
    Check(N.IsDecal(0xA) && N.IsDecal(0x8000048) && !N.IsDecal(0x10106) && !N.IsDecal(0xC) && !N.IsDecal(0x20), "decal is bit 0x8 with 0x4 clear (0xa and 0x8000048 are decals; 0x10106, 0xc and 0x20 are not)");
    Names Empty;
    Check(!Empty.Parse("# nothing\n") && !Empty.IsDecal(0x8), "a table with no rows decodes nothing");
    // the shipped files, when this build finds them
    for (const char* Profile : { "bo4", "t7", "t9" })
    {
        const Names* Shipped = For(Profile);
        Check(Shipped != nullptr, "the shipped flag_names.txt is found (zonekit/profiles next to the exe)", Profile);
        if (Shipped != nullptr) Check(Shipped->Count() == 39 && Shipped->Mask() == 0x03F00000u, "the shipped table holds the 39 surface types", std::string(Profile) + " " + std::to_string(Shipped->Count()));
    }
    const Names* Bo4 = For("bo4");
    Check(Bo4 != nullptr && Bo4->HasRenderBits(), "the bo4 table has the render bits");
    Check(ClassOf("bo4", 0x80000a) == RenderClass::Decal && ClassOf("bo4", 0x10106) == RenderClass::NotDecal && ClassOf("bo4", 0) == RenderClass::Unread &&
          ClassOf("bo4", -1) == RenderClass::Unread && ClassOf("t7", 0x80000a) == RenderClass::Unread, "ClassOf: decal, not decal; 0, -1 and a game with no render rows are unread");
    Check(ReadAndNamesNoSurface("bo4", 0x440A0) && ReadAndNamesNoSurface("bo3", 0) && ReadAndNamesNoSurface("coldwar", 0x440A0) && !ReadAndNamesNoSurface("bo4", -1) &&
          !ReadAndNamesNoSurface("bo4", 0xD00000) && !ReadAndNamesNoSurface("nogame", 0), "ReadAndNamesNoSurface: a read word with no type bits only; -1, a typed word and an unknown game are not");
    Materials::Source Src;
    Materials::Description D;
    Src.SurfaceFlags = -1;
    Apply("bo4", Src, D);
    Check(D.SurfaceType.empty() && D.SurfaceWhy == "not_read", "no word read: no type, not_read", D.SurfaceWhy);
    Src.SurfaceFlags = 0xD440A0;
    Apply("bo4", Src, D);
    Check(D.SurfaceType == "metal" && D.SurfaceWhy.empty(), "bo4 0xD440A0 is metal", D.SurfaceType);
    Apply("no_such_profile", Src, D);
    Check(D.SurfaceType.empty() && (D.SurfaceWhy == "no_table" || D.SurfaceWhy == "table_missing"), "a profile with no table gives no type and a reason", D.SurfaceWhy);
    const std::vector<std::string> Options = { "<error>", "<none>", "Metal", "wood" };
    Check(ToBo3("metal", Options) == "Metal" && ToBo3("wood", Options) == "wood" && ToBo3("ladder", Options).empty(), "ToBo3 matches an option by name, case aside, and nothing else");
    return Failed;
}
}

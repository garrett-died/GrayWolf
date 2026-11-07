#pragma once

// The GDT entries of a map's sky in the Black Ops III Mod Tools' own form (M4e, docs/hydrax-port/m4e.md): an image (the lat-long EXR), a material (materialType
// sky_latlong_hdr) and a sky xmodel whose skinOverride swaps the stock sky material for it, as the 58 lat-long skies of model_export/t7_skybox.gdt are written. The
// xmodel's filename names the stock sky mesh (t6_props\vista\skybox\t6_skybox.xmodel_bin, 76 of 80 stock xmodels): the model is never copied, only named. Brightness
// fields are the median of the stock sky materials and are labelled so in the report; the game's own value cannot be proven to match.
// Facts are target data (zonekit/profiles/t7/target/sky-stock.json), not code. Pure text builders; the Radiant writer decides where files go.

#include "GdtWriter.h"

#include <set>
#include <string>
#include <vector>

namespace Maps
{
    struct SkyStock
    {
        std::set<std::string> Names;       // every entry name of the stock sky GDT (image, material, xmodel)
        Gdt::Fields MaterialValues;        // the stock modal / median values of the sky-specific material keys
        Gdt::Fields ImageValues;           // the stock image keys
        // the whole stock entries (every key and value as the file has them) the sky entries start from: skybox_mp_chinatown (image, xmodel) and mtl_skybox_mp_chinatown (material)
        Gdt::Fields StockImage, StockMaterial, StockXmodel;
        std::string XmodelFile;            // the stock sky mesh (single backslashes)
        size_t Materials = 0;
        std::string Source;
        // the keys every stock material writes (the techset's slots empty, the modal value of the rest), from the material defaults and the sky_latlong_hdr techset
        Gdt::Fields MaterialBase;
    };
    // Reads sky-stock.json and, from the same profiles folder, the material defaults and techset catalog. False (Why) when a file is missing or holds nothing usable.
    bool LoadSkyStock(SkyStock& Out, std::string& Why);
    // The sky-stock.json text alone (the tests): MaterialBase stays empty
    bool ParseSkyStock(const std::string& Json, SkyStock& Out, std::string& Why);

    struct SkyNames
    {
        std::string Image, Material, Xmodel;   // skybox_<map>, mtl_skybox_<map>, skybox_<map>
    };
    // The names of a map's sky: the Mod Tools' convention, with "_gw" appended while a stock entry has the name. False when the map's name is not SafeText.
    bool SkyNamesFor(const std::string& Map, const SkyStock& Stock, SkyNames& Out);

    // The three entries as one .gdt file's text (CRLF line ends as APE writes; entries in ordinal name order: material, image, xmodel). BaseImage is the EXR's path from the Mod Tools root with single
    // backslashes. Rotation is the material's skyRotation. Empty when a name or the path is not SafeText.
    std::string SkyGdtText(const SkyNames& Names, const std::string& BaseImage, const SkyStock& Stock, double Rotation);

    int SkyGdtSelfTest(std::string& Failures);
}

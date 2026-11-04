#pragma once

#include <cstdint>
#include <memory>

class CoDAnim_t;
class CoDModel_t;
class CoDSound_t;
struct XImage_t;
struct XMaterial_t;
struct XModelLod_t;
struct XModel_t;
struct XAnim_t;
struct XImageDDS;
struct XSound;
class WraithModel;
class WraithNameIndex;

class GameBlackOpsCW
{
public:
    static WraithNameIndex AssetNameCache;
    static WraithNameIndex StringCache;

    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    static std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model);
    static std::unique_ptr<XSound> ReadXSound(const CoDSound_t* Sound);
    static const XMaterial_t ReadXMaterial(uint64_t MaterialPointer);
    // The material's name and image slots (semantic hash, image name) alone: ReadXMaterial also reads
    // its shaders and constants
    static const XMaterial_t ReadXMaterialImages(uint64_t MaterialPointer);
    static std::unique_ptr<XImageDDS> LoadXImage(const XImage_t& Image);
    static void LoadXModel(const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel);

    static void PerformInitialSetup();
};
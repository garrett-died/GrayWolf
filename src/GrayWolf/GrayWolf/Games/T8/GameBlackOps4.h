#pragma once

#include "ByteView.h"

#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <vector>

class CoDAnim_t;
class CoDModel_t;
struct XMaterial_t;
struct XModelLod_t;
struct XModel_t;
struct XAnim_t;
class WraithModel;
class WraithNameIndex;

class GameBlackOps4
{
public:
    static WraithNameIndex AssetNameCache;

    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    static std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model);
    static const XMaterial_t ReadXMaterial(uint64_t MaterialPointer);
    // What another zone gives a material header without images: its images (name hash, semantic
    // hash), its technique set's name hash and its constant blob.
    struct MaterialElsewhere
    {
        std::vector<std::pair<uint64_t, uint32_t>> Images;
        std::vector<uint32_t> Formats;                 // the DXGI format of each image (parallel to Images), 0 when the defining zone holds none
        uint64_t Techset = 0;
        std::vector<uint8_t> Constants;
        int64_t SurfaceFlags = -1, RenderFlags = -1;   // the material's surface flags (+0x28) and its techset's renderFlags (+0x20), -1 when unread
    };
    // A disk export's: a material header without images (another zone defines the material) ->
    // that zone's material; false when unknown. Unset outside a disk export.
    static thread_local std::function<bool(uint64_t MaterialHash, MaterialElsewhere& Out)> ImagesElsewhere;
    // The same hook asked for one image: the key is the image's 60-bit name hash with this bit set, and the answer is Out.Formats[0] (the DXGI
    // format of that image read wherever a header of it is, 0 when none is). The format is the image's own, so a material whose slot holds a stub
    // header (its image is defined in another zone) can still be told colour from mask. Not a second hook: one thread-local function is enough.
    static constexpr uint64_t ImageFormatKey = 1ull << 62;
    // A disk export's: the renderFlags of a technique set whose header in this zone is a stub (another zone holds it); 0 when unknown.
    static thread_local std::function<int64_t(uint64_t TechsetHash)> TechsetFlagsElsewhere;
    static void LoadXModel(const XModelLod_t& ModelLOD, const std::unique_ptr<WraithModel>& ResultModel);

    static std::string DecryptString(uint8_t* InputBuffer, uint8_t InputLength, uint8_t EncryptionID, uint64_t StringHash);

    static void PerformInitialSetup();

    // The on-disk Localize cipher, distinct from live memory's DecryptString: an arithmetic XOR
    // keystream seeded by the on-disk EncryptionID byte, no DecryptionTable. Only 4 EncryptionID
    // values are known (unknown IDs return false); a rare 2-byte anomaly in some longer strings is unexplained.
    static bool DecryptDiskString(const uint8_t* Ciphertext, uint8_t Length, uint8_t EncryptionID, std::string& OutText);
    // Offline Localize export: scans a decompressed zone buffer for Localize headers
    // (Value==T8FollowsInline, pad==0) and decrypts each one via DecryptDiskString.
    static bool ExportDiskLocalize(ByteView Zone, const std::string& OutPath, std::string& Report);
    // A second header shape, 24 bytes: Value==T8FollowsInline(8) + Hash(8) + AlwaysZero(8), Hash/Pad
    // swapped from LocalizeEntry's on-disk order. The string that follows has no
    // [EncryptionID][StringSize] prefix and no cipher; likely T8XAssetType_RAWSTRING (157), unconfirmed.
    static bool ExportDiskRawStrings(ByteView Zone, const std::string& OutPath, std::string& Report);
};
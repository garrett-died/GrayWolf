#pragma once

// The model preview's image based light: the procedural studio sky (ModelPreviewGPU.cpp) or an
// environment baked from an HDR panorama (bake-environment, below), turned about the up axis and
// scaled; the split-sum BRDF table the pixel shader's specular reads (with GGX's multiple
// scattering made up for, Fdez-Aguera 2019).
//
// A baked environment is two DDS cubes beside the exe, environments\<id>_radiance.dds (GGX
// prefiltered: mip m is roughness m / (mips - 1)) and <id>_irradiance.dds (the cosine-weighted
// light over pi), BC6H. Their sources and licenses: environments\NOTICE.txt.

#include <d3d11.h>
#include <directxtk/Effects.h>   // PBREffect (in an inline namespace: not declared ahead)
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct PreviewLighting;

// The id the procedural sky goes by (the default)
inline constexpr const char* kStudioEnvironment = "studio";

struct PreviewEnvironmentInfo
{
    std::string Id, Name;   // Id: the files' prefix ("studio": the procedural sky); Name: shown
};
// The procedural sky first, then each environment found beside the exe (read once)
const std::vector<PreviewEnvironmentInfo>& PreviewEnvironments();

// HLSL the preview's pixel shader is compiled after: the environment's constants (b3), the BRDF
// table (t24) and the functions that light a surface with them (EnvDir, EnvBrdf, EnvEnergy,
// EnvImageLight).
const char* PreviewEnvironmentHlsl();

// One renderer's environment: the cubes bound (PBREffect binds them at t4, t5), the BRDF table and
// the constants. Only the device is kept.
class PreviewEnvironment
{
public:
    // The procedural sky's cubes, the fallback when an environment cannot be read
    PreviewEnvironment(ID3D11Device* Dev, ID3D11ShaderResourceView* StudioRadiance, int StudioMips,
                       ID3D11ShaderResourceView* StudioIrradiance);
    ~PreviewEnvironment();
    PreviewEnvironment(const PreviewEnvironment&) = delete;
    PreviewEnvironment& operator=(const PreviewEnvironment&) = delete;
    bool Ok() const;

    // L's environment loaded when it changed, set on Effect (before its Apply), the constants and
    // the table bound to the pixel stage
    void Apply(ID3D11DeviceContext* Ctx, DirectX::PBREffect& Effect, const PreviewLighting& L);
    // The radiance cube Apply set last and its mips (the backdrop samples it)
    ID3D11ShaderResourceView* Radiance() const;
    int RadianceMips() const;

private:
    struct Impl;
    std::unique_ptr<Impl> P;
};

// A light source in memory, linear RGBA floats (alpha unused): one equirectangular panorama (Z up, +X its middle column,
// +Y to its left), or six cube faces of the Mod Tools' naming, Width == Height each, stored ft bk lf rt up dn one after
// the other. The axes of the six faces are in docs/hydrax-port/sky-axes.md.
struct EnvironmentImage
{
    enum class Layout { LatLong, CubeFaces };
    Layout Kind = Layout::LatLong;
    int Width = 0, Height = 0;
    std::vector<float> Rgba;   // LatLong: Width * Height * 4. CubeFaces: 6 * Width * Height * 4
};

// What a long bake is told: stop at the next row (Cancel), report 0..1 from the bake's thread (Progress), cut each source
// texel to ClampTexel before anything else (0: not; a sky's sun disc would otherwise light the model twice, with the
// sun), and scale the light by ScaleMultiplier beyond the studio sky's mean.
struct EnvironmentBakeControl
{
    const std::atomic<bool>* Cancel = nullptr;
    std::function<void(float)> Progress;
    float ClampTexel = 0.0f;
    float ScaleMultiplier = 1.0f;
};

// The source seen as a cube of Size per face in D3D's face order (+X -X +Y -Y +Z -Z), RGBA floats, 6 * Size * Size * 4,
// unscaled: the first stage of the bake, kept apart so the axes can be tested without a GPU.
bool EnvironmentSourceCube(const EnvironmentImage& Image, int Size, std::vector<float>& Rgba);

// Bakes Image into OutDir\<Id>_radiance.dds and <Id>_irradiance.dds as the .hdr overload does (that one loads the file and
// calls this). Fails with reason=cancelled when Control.Cancel was set.
bool BakePreviewEnvironment(const EnvironmentImage& Image, const std::string& OutDir, const std::string& Id, int Size, std::string& Report,
                            const EnvironmentBakeControl& Control = EnvironmentBakeControl());

// Environments beyond those beside the exe (the Mod Tools skies baked into the cache): a baked environment is also
// looked for in this folder. Set at start; empty: none.
void SetExtraEnvironmentDir(const std::string& Dir);

// Bakes HdrPath (an equirectangular .hdr, Z up) into OutDir\<Id>_radiance.dds and
// <Id>_irradiance.dds, Size the radiance cube's face, its light scaled so its mean matches the
// procedural sky's. Report: what was written, the scale, the time.
bool BakePreviewEnvironment(const std::string& HdrPath, const std::string& OutDir, const std::string& Id, int Size,
                            std::string& Report);

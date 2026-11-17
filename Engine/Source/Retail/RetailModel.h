#ifndef ENGINE_SOURCE_RETAIL_RETAILMODEL_H
#define ENGINE_SOURCE_RETAIL_RETAILMODEL_H

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <vector>

namespace Retail {

struct AnimationFile {
    uint16_t id;
    uint16_t variation;
    uint32_t fileId;
};

struct AnimationSpan {
    size_t offset = 0, size = 0;
    bool present = false;
};

struct AnimationLayout {
    bool chunked = false;
    AnimationSpan model, bones, attachments;
};

// Chunk offsets are independent address spaces: AFM2 model tracks, AFSB skeleton bones,
// AFSA skeleton attachments. Legacy flat files use one span for all three.
bool InspectAnimation(const uint8_t* bytes, size_t size, AnimationLayout& layout, std::string& error);

struct ModelFiles {
    uint32_t version = 0;
    uint32_t skeleton = 0;
    std::vector<uint32_t> skins;
    std::vector<uint32_t> textures;
    std::vector<AnimationFile> animations;
};

struct HelmetAnchor {
    std::array<float, 3> head{};
    std::array<float, 3> socket{};
    std::array<float, 3> jaw{};     // key bone 7, for the head's size
    bool hasJaw = false;
};

// Read head key-bone 6 and attachment 11 from an unrelocated, flat model. Used to retain
// installed helmet origins when a player body uses a different skeleton/socket convention.
bool ReadHelmetAnchor(const std::vector<uint8_t>& model, HelmetAnchor& anchor, std::string& error);
std::array<float, 3> HelmetTranslation(const HelmetAnchor& donor, const HelmetAnchor& target);

// All offsets remain file-relative. Preparation happens before the existing M2 relocation
// pass; cached CDN bytes are never changed. Unsupported features fail with a reason.
bool InspectModel(const std::vector<uint8_t>& bytes, ModelFiles& files, std::string& error);
// Flatten one already-resolved parent into a child SKEL. Resident offsets are
// rebuilt; external tracks keep their owning AFID. Inputs are never modified.
bool MergeSkeleton(const std::vector<uint8_t>& child, const std::vector<uint8_t>& parent,
    std::vector<uint8_t>& merged, std::string& error);
bool PrepareModel(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& prepared,
    ModelFiles& files, std::string& error, const std::vector<uint8_t>* skeleton = nullptr);
// A skeleton that inherits sequences from a parent (Void Elf from Blood Elf) gets them appended after
// its own, but the model's event tracks (sheathe, footsteps, spell casts) list only the child's own
// sequences. Extend each event track to every sequence, taking the parent model's times for the
// inherited ones (matched by event identifier and sequence id/variation). `filled` counts the timelines
// added. Call on PrepareModel's output.
bool InheritEvents(std::vector<uint8_t>& prepared, const std::vector<uint8_t>& parentModel,
    const std::vector<uint8_t>& parentSkeleton, uint32_t& filled, std::string& error);
bool ValidateSkin(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& model,
    std::string& error);
// Retail's packed shader ids (0x8000 + an index into retail's shader table) keep their index in the
// loaded skin, tagged so the stock code can't misread them: 3.3.5 numbers its own three packed effects
// from 1 and uses 0x8000 itself as "disabled layer" (CM2Shared::InitializeSkinProfile, the draw paths).
// PrepareSkin writes kRetailPackedTag | index into the batch (only in memory, never into a file), and
// CM2Shared::InitializeSkinProfile does for the ids from 0x8001 a 3.3.5-format model stores (retroports);
// CM2Shared::GetEffect draws it by RetailPackedShader(index), and the Batches panel shows it as retail's id.
constexpr uint16_t kRetailPackedTag = 0xA000;   // 0x8000 kept (a packed id), 0x2000 marks retail numbering
constexpr uint16_t kRetailPackedMask = 0xF000;
constexpr uint16_t kRetailIndexMask = 0x0FFF;

inline bool IsRetailPacked(uint16_t shader) { return (shader & kRetailPackedMask) == kRetailPackedTag; }
inline uint16_t RetailPackedId(uint16_t shader) { return uint16_t(0x8000 | (shader & kRetailIndexMask)); }

// One entry of retail's packed shader table: retail's own shaders, and the stand-in that draws it when
// they can't be used.
struct RetailShader {
    const char* vertex;         // retail's shaders: Cataclysm's table (4.3.4), drawn with Cataclysm's own
    const char* pixel;          // shader files where 3.3.5 lacks them (Tools/VulkanShaders --cata); nullptr
                                // past what we know
    uint8_t textures;           // the stages retail's shaders sample
    const char* standInVertex;  // the stock shaders that draw it when retail's are missing, or the batch
    const char* standInPixel;   // has fewer textures than they sample
    const char* missing;        // what the stand-in lacks (nullptr: nothing)
    uint8_t standInTextures;    // the stages the stand-in samples; a batch with fewer gets the
                                // first-texture stand-in instead
    const char* unavailable = nullptr;  // why retail's shaders are never used here (they need what 3.3.5
                                        // doesn't have), else nullptr
    bool nameUnknown = false;   // past Cataclysm's table: vertex / pixel draw what it does; its name is unknown
};

// The entry for packed index `index` (retail id 0x8000 + index). Never null: an index we know nothing
// about gets the first-texture stand-in, named as unknown.
const RetailShader& RetailPackedShader(uint16_t index);
// Whether `index` has an entry of its own (else RetailPackedShader returns the unknown stand-in).
bool IsKnownRetailPacked(uint16_t index);
// The first-texture stand-in: Diffuse_T1 + Combiners_Mod, which keeps the texture's alpha (Combiners_Opaque
// drew an additive headlight beam as a solid panel; Esteria's wowdecomp found it).
const RetailShader& RetailFirstTextureStandIn();
// The texture transforms a retail model vertex shader reads, as Cataclysm sets them
// (CM2SceneRender::SetupTextureTransforms there): up to two {transform combo slot, matrix stage} pairs,
// in order, -1 where unused. Env stages read no transform.
struct RetailTransforms { int8_t slot[2]; int8_t stage[2]; };
RetailTransforms RetailVertexTransforms(const char* vertexShader);
// Non-packed retail ids (stage ops in the low byte, 0x4000 = second UV set) mean what they mean in
// 3.3.5 and stay as they are; the stock CM2Shared::GetEffect(count, shader, coords) decodes them.
bool PrepareSkin(std::vector<uint8_t>& bytes, const std::vector<uint8_t>& model,
    std::string& error, bool degradeShaders = false);
// Unwrap model-only AFM2 files. Preserve split skeleton chunks for category-specific relocation.
bool StripAnimationChunk(std::vector<uint8_t>& bytes, std::string& error);
bool ValidateAnimation(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& model,
    uint16_t id, uint16_t variation, std::string& error);
std::string FileName(uint32_t fileId, const char* extension);

}
#endif

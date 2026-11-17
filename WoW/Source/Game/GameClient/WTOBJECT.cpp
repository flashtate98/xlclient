#include "WTOBJECT.h"
#include "Engine/Source/Base/CDataAllocator.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Tempest/CMath.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxVertexPC.h"
#include "Engine/Source/Gx/CGxVertexPCT.h"
#include "Engine/Source/Model2/M2Log.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "Engine/Source/Retail/LocalSource.h"
#include "Engine/Source/Services/Texture.h"
#include <string>
#include <unordered_map>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "WoW/Source/WorldClient/FrameTime.h"
#include <typeinfo>

// STATIC: 0x00D38074
static uint32_t s_count;

// STATIC: 0x00D38078
static CDataAllocator s_freeSwings(sizeof(SWING), 8);

// STATIC: 0x00D3808C
static CDataAllocator s_freeTrails(sizeof(WTOBJECT), 128);

// INLINED
WTOBJECT::~WTOBJECT() {
    if (this->m_model) {
        CM2Model* model = this->m_model;
        model->m_fFlags |= 0x20;
        model->m_drawCallback = nullptr;
        model->m_drawArg = nullptr;
        this->m_model->Release();
    }

    if (this->m_swing) {
        s_freeSwings.PutData(this->m_swing, nullptr, 0);
    }
}

// VERIFY: c3c7b136 auto
// OFFSET: 0x007E4BC0
void WeaponTrailsShutdown() {
    s_freeTrails.Clear(typeid(WTOBJECT).raw_name(), SLOG_OBJECT, 1);
    s_freeSwings.Clear(typeid(SWING).raw_name(), SLOG_OBJECT, 1);
}

// VERIFY: 38b83c0d auto
// OFFSET: 0x007E4BF0
WTOBJECT* WeaponTrailCreate(CM2Model* model) {
    s_count++;
    WTOBJECT* trail = static_cast<WTOBJECT*>(s_freeTrails.GetData(0, typeid(WTOBJECT).raw_name(), SLOG_OBJECT));

    if (trail) {
        trail->m_swing = nullptr;
        trail->m_model = nullptr;
    }

    trail->m_model = model;
    model->m_refCount++;
    return trail;
}

// VERIFY: 254754a7 auto
// OFFSET: 0x007E4CC0
void WeaponTrailClose(WTOBJECT* trail) {
    s_count--;
    s_freeTrails.Delete(trail);
}

// VERIFY: 918160ea manual
// OFFSET: 0x007E4B70
void SWING::FillVertices(CImVector color, uint8_t fade, CGxVertexPC* vertices) {
    for (int32_t i = this->m_tail; i < this->m_head; i++, vertices++) {
        vertices->p = this->m_points[i & 0x7F];

        if (!(i & 0x1)) {
            color.a -= fade;
        }

        vertices->c = color;
    }
}

// VERIFY: b7752821 manual
// OFFSET: 0x007E4C30
SWING::SWING() {
    this->m_tail = 0;
    this->m_head = 0;
    *reinterpret_cast<uint32_t*>(&this->m_color) = 0;
    this->m_texture = nullptr;
}

// Not in the original: FlashWOW. A Retail trail texture by FileDataID, loaded once and kept.
static CTexture* WeaponTrailTexture(uint32_t fileDataId) {
    static std::unordered_map<uint32_t, CTexture*> s_textures;

    const auto found = s_textures.find(fileDataId);

    if (found != s_textures.end()) {
        return found->second;
    }

    std::string path;
    CTexture* texture = nullptr;

#if defined(WOWDECOMP_RETAIL_MODELS)
    // The listfile lives in the Retail loader; without it (WOWDECOMP_RETAIL_MODELS off) a trail has no texture.
    if (Retail::Source().PathOf(fileDataId, path)) {
        CStatus status;
        texture = TextureCreate(path.c_str(), CGxTexFlags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1, 0, 0, 0), &status, 0);
    }
#endif

    M2Log("Weapon trail texture %u: %s%s\n", fileDataId, path.empty() ? "no path in the listfile" : path.c_str(),
        texture ? "" : " (not loaded)");
    s_textures[fileDataId] = texture;
    return texture;
}

// Not in the original: FlashWOW Retail weapon trails. Retail's trail is a ribbon of the blade's recent
// positions: each frame of the swing adds an edge (the blade from bottom to tip, stamped with the time), an
// edge lives kEdgeLifeMs, and the strip between them is smoothed (Catmull-Rom, kSteps pieces between two
// edges) so a fast swing draws an arc rather than a fan of straight cuts. The texture is laid along the
// strip by age: U is 0 at the blade and 1 at an edge about to die, V is 0 at the tip and 1 at the bottom,
// the way Retail's trail textures are painted (bright at the blade and along the tip, fading behind).
// Drawn added at full strength (one, one) like Retail's glowing trails; the colour's alpha scales it.
static const uint32_t kEdgeLifeMs = 500;
static const uint32_t kSteps = 8;
static const uint32_t kSmoothPasses = 2;

static C3Vector CatmullRom(const C3Vector& p0, const C3Vector& p1, const C3Vector& p2, const C3Vector& p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
}

bool SWING::BuildTexturedStrip(uint32_t time, const C3Vector& top, const C3Vector& bottom) {
    const uint32_t elapsed = time - this->m_startTime;
    this->m_lastTime = time;

    // Edges are pairs in m_points (bottom, top), at most kMaxEdges, their times in m_edgeTimes.
    if (elapsed < this->m_duration) {
        if (this->m_head - this->m_tail >= 2 * static_cast<int32_t>(kMaxEdges)) {
            this->m_tail += 2;
        }

        const int32_t edge = this->m_head / 2;
        this->m_points[this->m_head++ & 0x7F] = bottom;
        this->m_points[this->m_head++ & 0x7F] = top;
        this->m_edgeTimes[edge % kMaxEdges] = time;
    }

    while (this->m_tail < this->m_head && time - this->m_edgeTimes[(this->m_tail / 2) % kMaxEdges] >= kEdgeLifeMs) {
        this->m_tail += 2;
    }

    const int32_t edges = (this->m_head - this->m_tail) / 2;

    if (edges <= 0) {
        return elapsed < this->m_duration;   // nothing yet, or the trail has died out
    }

    if (edges < 2) {
        return true;
    }

    CGxTex* gxTex = TextureGetGxTex(this->m_texture, 0, nullptr);

    if (!gxTex) {
        return true;
    }

    // Newest edge first: the strip starts at the blade. The recorded edges carry the swing animation's
    // frame-to-frame wobble (the wrist turning a little each way), so the strip is drawn through a smoothed
    // copy: kSmoothPasses passes of a 1-2-1 average over neighbouring edges. The newest edge stays exact, so
    // the trail still leaves from the blade.
    const int32_t first = this->m_head / 2 - 1;
    C3Vector smooth[2][kMaxEdges];

    for (int32_t k = 0; k < edges; k++) {
        smooth[0][k] = this->m_points[(2 * (first - k)) & 0x7F];
        smooth[1][k] = this->m_points[(2 * (first - k) + 1) & 0x7F];
    }

    for (uint32_t pass = 0; pass < kSmoothPasses; pass++) {
        for (int32_t side = 0; side < 2; side++) {
            C3Vector previous = smooth[side][0];

            for (int32_t k = 1; k + 1 < edges; k++) {
                const C3Vector current = smooth[side][k];
                smooth[side][k] = (previous + current * 2.0f + smooth[side][k + 1]) * 0.25f;
                previous = current;
            }
        }
    }

    auto edgeAt = [&](int32_t k, bool tip) -> const C3Vector& {
        k = k < 0 ? 0 : (k >= edges ? edges - 1 : k);
        return smooth[tip ? 1 : 0][k];
    };
    auto ageAt = [&](int32_t k) -> float {
        return static_cast<float>(time - this->m_edgeTimes[(first - k) % kMaxEdges]);
    };

    const uint32_t count = (static_cast<uint32_t>(edges - 1) * kSteps + 1) * 2;
    CImVector color = this->m_color;
    const float strength = color.a / 255.0f;
    color.r = static_cast<uint8_t>(color.r * strength);
    color.g = static_cast<uint8_t>(color.g * strength);
    color.b = static_cast<uint8_t>(color.b * strength);
    color.a = 255;
    color = GxSwizzleColor(color);

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_BlendingMode, GxBlend_NoAlphaAdd);
    GxRsSet(GxRs_AlphaRef, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_DepthTest, 1);
    g_theGxDevicePtr->RsSet(GxRs_Texture0, gxTex);
    GxRsSet(GxRs_Texture1, 0);
    GxRsSet(GxRs_ColorOp0, GxTexOp_Mod);
    GxRsSet(GxRs_AlphaOp0, GxTexOp_Mod);
    GxRsSet(GxRs_TextureShader0, 0);
    GxRsSet(GxRs_TexGen0, GxTexGen_Disabled);

    CGxBuf* buf = g_theGxDevicePtr->BufStream(GxPoolTarget_Vertex, sizeof(CGxVertexPCT), count);
    CGxVertexPCT* vertices = reinterpret_cast<CGxVertexPCT*>(g_theGxDevicePtr->BufLock(buf));

    for (int32_t k = 0; k < edges; k++) {
        const uint32_t steps = k + 1 < edges ? kSteps : 1;

        for (uint32_t step = 0; step < steps; step++) {
            const float t = static_cast<float>(step) / static_cast<float>(kSteps);
            const float age = ageAt(k) + (ageAt(k + 1 < edges ? k + 1 : k) - ageAt(k)) * t;
            const float u = age / static_cast<float>(kEdgeLifeMs);

            for (int32_t side = 0; side < 2; side++) {
                const bool tip = side == 0;
                vertices->p = CatmullRom(edgeAt(k - 1, tip), edgeAt(k, tip), edgeAt(k + 1, tip), edgeAt(k + 2, tip), t);
                vertices->c = color;
                vertices->tc.x = u > 1.0f ? 1.0f : u;
                vertices->tc.y = tip ? 0.0f : 1.0f;
                vertices++;
            }
        }
    }

    g_theGxDevicePtr->BufUnlock(buf, 0);
    buf->m_hasData = 1;
    GxPrimVertexPtr(buf, GxVBF_PCT);

    CGxBatch batch;
    batch.m_primType = GxPrim_TriangleStrip;
    batch.m_start = 0;
    batch.m_count = count;
    batch.m_minIndex = 0;
    batch.m_maxIndex = count - 1;
    g_theGxDevicePtr->Draw(&batch, 0);
    g_theGxDevicePtr->RsPop();
    return true;
}

// VERIFY: 76207940 manual
// OFFSET: 0x007E4CE0
bool SWING::BuildVertexBuffer(uint32_t time, const C3Vector& top, const C3Vector& bottom) {
    if (this->m_texture) {   // Not in the original: FlashWOW
        return this->BuildTexturedStrip(time, top, bottom);
    }

    uint32_t elapsed = time - this->m_startTime;

    if (elapsed < this->m_duration) {
        int32_t index = this->m_head++;

        if (this->m_tail < this->m_head - 128) {
            this->m_tail = this->m_head - 128;
        }

        this->m_points[index & 0x7F] = bottom;
        index = this->m_head++;

        if (this->m_tail < this->m_head - 128) {
            this->m_tail = this->m_head - 128;
        }

        this->m_points[index & 0x7F] = top;
    }

    uint8_t alpha = this->m_color.a;
    int32_t delta = time - this->m_lastTime;
    this->m_lastTime = time;
    uint8_t fade = static_cast<uint8_t>(FloatToInt64(delta * 0.0033333334f * alpha));

    if (!fade) {
        fade = 1;
    }

    int32_t segments = alpha / fade;

    if (segments <= 1) {
        return false;
    }

    int32_t head = this->m_head;

    if (this->m_tail < head - 2 * segments) {
        this->m_tail = head - 2 * segments;
    }

    int32_t count = head - this->m_tail;

    if (!count) {
        return false;
    }

    if (count >= 3) {
        g_theGxDevicePtr->RsPush();
        GxRsSet(GxRs_Culling, 0);
        GxRsSet(GxRs_Fog, 0);
        GxRsSet(GxRs_Lighting, 0);
        GxRsSet(GxRs_BlendingMode, GxBlend_Alpha);
        GxRsSet(GxRs_AlphaRef, static_cast<int32_t>(CGxDevice::s_alphaRef[g_theGxDevicePtr->m_appRenderStates.m_data[GxRs_BlendingMode].m_value.m_data.i[0]]));
        GxRsSet(GxRs_DepthWrite, 0);
        GxRsSet(GxRs_DepthTest, 1);
        GxRsSet(GxRs_Texture0, 0);
        GxRsSet(GxRs_Texture1, 0);

        CGxBuf* buf = g_theGxDevicePtr->BufStream(GxPoolTarget_Vertex, sizeof(CGxVertexPC), count);
        CGxVertexPC* vertices = reinterpret_cast<CGxVertexPC*>(g_theGxDevicePtr->BufLock(buf));
        this->FillVertices(GxSwizzleColor(this->m_color), fade, vertices);

        if (static_cast<int32_t>(elapsed) > static_cast<int32_t>(this->m_duration >> 1)) {
            this->m_color.a -= fade;
        }

        g_theGxDevicePtr->BufUnlock(buf, 0);
        buf->m_hasData = 1;
        GxPrimVertexPtr(buf, GxVBF_PC);

        CGxBatch batch;
        batch.m_primType = GxPrim_TriangleStrip;
        batch.m_start = 0;
        batch.m_count = count;
        batch.m_minIndex = 0;
        batch.m_maxIndex = count - 1;
        g_theGxDevicePtr->Draw(&batch, 0);
        g_theGxDevicePtr->RsPop();
    }

    return true;
}

// VERIFY: 1e058dc3 auto
// OFFSET: 0x007E4F50
static void WeaponTrailCallback(CM2Model* model, const CM2Lighting* lighting, void* arg) {
    if (!model->HasEvent('BTW$') || !model->HasEvent('TTW$')) {
        return;
    }

    uint32_t time = FrameTime::s_curTimeMs;
    C3Vector bottom = model->GetEventPosition('BTW$');
    C3Vector top = model->GetEventPosition('TTW$');
    WTOBJECT* trail = static_cast<WTOBJECT*>(arg);

    if (!trail->m_swing->BuildVertexBuffer(time, top, bottom)) {
        s_freeSwings.PutData(trail->m_swing, nullptr, 0);
        trail->m_swing = nullptr;
        model->m_fFlags |= 0x20;
        model->m_drawCallback = nullptr;
        model->m_drawArg = nullptr;
    }
}

// VERIFY: 2687134f auto
// OFFSET: 0x007E4FF0
void WTOBJECT::SetDrawTrail(const uint32_t* color, uint32_t duration, uint32_t textureFileDataId) {
    if (this->m_swing) {
        this->m_swing->m_tail = 0;
        this->m_swing->m_head = 0;
    } else {
        SWING* swing = static_cast<SWING*>(s_freeSwings.GetData(0, typeid(SWING).raw_name(), SLOG_OBJECT));

        if (swing) {
            new (swing) SWING();
        }

        CM2Model* model = this->m_model;
        this->m_swing = swing;
        model->m_fFlags |= 0x20;
        model->m_drawCallback = WeaponTrailCallback;
        model->m_drawArg = this;
    }

    *reinterpret_cast<uint32_t*>(&this->m_swing->m_color) = *color;
    this->m_swing->m_texture = textureFileDataId ? WeaponTrailTexture(textureFileDataId) : nullptr;   // FlashWOW
    this->m_swing->m_duration = duration;
    this->m_swing->m_startTime = FrameTime::s_curTimeMs;
    this->m_swing->m_lastTime = this->m_swing->m_startTime;
}

// VERIFY: b37df885 auto
// OFFSET: 0x007E5080
void WeaponTrailSetDrawTrail(WTOBJECT* trail, const uint32_t* a2, uint32_t a3, uint32_t textureFileDataId) {
    if (a3 && trail) {
        trail->SetDrawTrail(a2, a3, textureFileDataId);
    }
}

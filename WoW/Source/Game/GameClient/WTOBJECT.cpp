#include "WTOBJECT.h"
#include "Engine/Source/Base/CDataAllocator.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Tempest/CMath.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxVertexPC.h"
#include "Engine/Source/Gx/CGxVertexPCT.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Log.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "Engine/Source/Retail/LocalSource.h"
#include "Engine/Source/Services/Texture.h"
#include <cmath>
#include <cstdlib>
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

// Not in the original: FlashWOW Retail weapon trails. The textured strip: U runs along the trail (0 at the
// weapon, 1 at the oldest edge kept), V across it (0 at the top point, the blade's tip, 1 at the bottom),
// as Retail's trail textures are drawn: brightest at the weapon and along the tip. The texture fades the
// trail along its length itself, so the colour is the same over the strip (it fades as a whole).
void SWING::FillVertices(CImVector color, uint8_t fade, CGxVertexPCT* vertices) {
    const int32_t edges = (this->m_head - this->m_tail) / 2;

    for (int32_t i = this->m_tail; i < this->m_head; i++, vertices++) {
        vertices->p = this->m_points[i & 0x7F];
        vertices->c = color;
        const int32_t edge = (this->m_head - 1 - i) / 2;
        vertices->tc.x = edges > 1 ? static_cast<float>(edge) / static_cast<float>(edges - 1) : 0.0f;
        vertices->tc.y = (i & 0x1) ? 0.0f : 1.0f;   // odd points are the tops
    }
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

    if (Retail::Source().PathOf(fileDataId, path)) {
        CStatus status;
        texture = TextureCreate(path.c_str(), CGxTexFlags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1, 0, 0, 0), &status, 0);
    }

    M2Log("Weapon trail texture %u: %s%s\n", fileDataId, path.empty() ? "no path in the listfile" : path.c_str(),
        texture ? "" : " (not loaded)");
    s_textures[fileDataId] = texture;
    return texture;
}

// VERIFY: 76207940 manual
// OFFSET: 0x007E4CE0
bool SWING::BuildVertexBuffer(uint32_t time, const C3Vector& top, const C3Vector& bottom) {
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

        // Not in the original: FlashWOW. A textured (Retail) trail: the texture times the colour, added.
        CGxTex* gxTex = this->m_texture ? TextureGetGxTex(this->m_texture, 0, nullptr) : nullptr;

        if (gxTex) {
            GxRsSet(GxRs_BlendingMode, GxBlend_Add);
            GxRsSet(GxRs_AlphaRef, 0);
            g_theGxDevicePtr->RsSet(GxRs_Texture0, gxTex);
            GxRsSet(GxRs_ColorOp0, GxTexOp_Mod);
            GxRsSet(GxRs_AlphaOp0, GxTexOp_Mod);
            GxRsSet(GxRs_TextureShader0, 0);
            GxRsSet(GxRs_TexGen0, GxTexGen_Disabled);
        }

        const uint32_t vertexSize = gxTex ? sizeof(CGxVertexPCT) : sizeof(CGxVertexPC);
        CGxBuf* buf = g_theGxDevicePtr->BufStream(GxPoolTarget_Vertex, vertexSize, count);
        void* locked = g_theGxDevicePtr->BufLock(buf);

        if (gxTex) {
            this->FillVertices(GxSwizzleColor(this->m_color), fade, static_cast<CGxVertexPCT*>(locked));
        } else {
            this->FillVertices(GxSwizzleColor(this->m_color), fade, static_cast<CGxVertexPC*>(locked));
        }

        if (static_cast<int32_t>(elapsed) > static_cast<int32_t>(this->m_duration >> 1)) {
            this->m_color.a -= fade;
        }

        g_theGxDevicePtr->BufUnlock(buf, 0);
        buf->m_hasData = 1;
        GxPrimVertexPtr(buf, gxTex ? GxVBF_PCT : GxVBF_PC);

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

// Not in the original: FlashWOW. A weapon without the trail events ($BTW / $TTW: many Retail weapons): the
// blade taken as the model's longest extent from its origin (the grip), the trail from a quarter of the way
// up to the tip, through the root bone.
static bool WeaponTrailFallbackPoints(CM2Model* model, C3Vector& top, C3Vector& bottom) {
    const M2Data* data = model->m_shared->m_data;

    if (!data->m_bones.m_count || !model->m_boneMatrices) {
        return false;
    }

    const CAaBox& box = data->m_bounds.m_extent;
    const float lows[3] = { box.b.x, box.b.y, box.b.z };
    const float highs[3] = { box.t.x, box.t.y, box.t.z };
    uint32_t axis = 0;
    float reach = 0.0f;

    for (uint32_t i = 0; i < 3; i++) {
        const float extent = fabsf(lows[i]) > fabsf(highs[i]) ? lows[i] : highs[i];

        if (fabsf(extent) > fabsf(reach)) {
            reach = extent;
            axis = i;
        }
    }

    if (fabsf(reach) < 0.05f) {
        return false;
    }

    float tip[3] = { 0.0f, 0.0f, 0.0f };
    tip[axis] = reach;
    float base[3] = { 0.0f, 0.0f, 0.0f };
    base[axis] = reach * 0.25f;

    model->Animate();
    top = C3Vector(tip[0], tip[1], tip[2]) * model->m_boneMatrices[0] * model->m_scene->m_viewInv;
    bottom = C3Vector(base[0], base[1], base[2]) * model->m_boneMatrices[0] * model->m_scene->m_viewInv;
    return true;
}

// VERIFY: 1e058dc3 auto
// OFFSET: 0x007E4F50
static void WeaponTrailCallback(CM2Model* model, const CM2Lighting* lighting, void* arg) {
    C3Vector bottom;
    C3Vector top;
    const bool events = model->HasEvent('BTW$') && model->HasEvent('TTW$');

    if (events) {
        bottom = model->GetEventPosition('BTW$');
        top = model->GetEventPosition('TTW$');
    } else if (!WeaponTrailFallbackPoints(model, top, bottom)) {
        return;
    }

    static std::unordered_map<const void*, bool> s_logged;   // once per model file

    if (std::getenv("WOW_FX_LOG") && !s_logged[model->m_shared]) {
        s_logged[model->m_shared] = true;
        M2Log("Weapon trail on %s: %s\n", model->m_shared->m_fileNameWithoutPath,
            events ? "the model's trail events" : "no trail events, the model's longest extent");
    }

    uint32_t time = FrameTime::s_curTimeMs;
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

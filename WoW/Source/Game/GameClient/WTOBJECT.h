#ifndef WOW_SOURCE_GAME_GAMECLIENT_WTOBJECT_H
#define WOW_SOURCE_GAME_GAMECLIENT_WTOBJECT_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CImVector.h"

class CM2Model;
class CGxVertexPC;
class CGxVertexPCT;
class CTexture;

struct SWING {
    /* 0000 */ int32_t m_head;
    /* 0004 */ int32_t m_tail;
    /* 0008 */ C3Vector m_points [128];
    /* 0608 */ CImVector m_color;
    /* 060C */ uint32_t m_duration;
    /* 0610 */ uint32_t m_startTime;
    /* 0614 */ uint32_t m_lastTime;
    CTexture* m_texture;    // FlashWOW: a Retail textured trail (not owned: WeaponTrailTexture keeps it)

    SWING();
    void FillVertices(CImVector color, uint8_t fade, CGxVertexPC* vertices);
    void FillVertices(CImVector color, uint8_t fade, CGxVertexPCT* vertices);   // FlashWOW
    bool BuildVertexBuffer(uint32_t time, const C3Vector& top, const C3Vector& bottom);
};

class WTOBJECT {
    public:
    /* 0000 */ SWING* m_swing;
    /* 0004 */ CM2Model* m_model;

    ~WTOBJECT();
    void SetDrawTrail(const uint32_t* color, uint32_t duration, uint32_t textureFileDataId = 0);
};

WTOBJECT* WeaponTrailCreate(CM2Model* model);
void WeaponTrailClose(WTOBJECT* trail);
void WeaponTrailsShutdown();
void WeaponTrailSetDrawTrail(WTOBJECT* trail, const uint32_t* a2, uint32_t a3, uint32_t textureFileDataId = 0);

void WorldTextInitialize();
void WorldTextShutdown();

#endif

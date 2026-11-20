// SXC: the body texture of retail (MD21) character models (kb modern-models).
//
// Retail bodies' UVs expect retail's sheet (CharComponentTextureSections, layouts 2 and 103+: 2048 x 1024):
// the eight body sections are the 3.3.5 grid at twice the size on the left half, and both face sections
// cover the whole right half (one square face texture instead of 3.3.5's two strips). Their textures are
// DXT BLPs, which the stock paste can't read (it fills green). For a retail body the compositor keeps its
// square texture, holds retail's layout in it (squeezed sideways: UVs are 0..1 either way) and pastes
// through here: any BLP encoding, scaled into the section, every mip level.
#pragma once

#include <cstdint>
#include "WoW/Common/TextureCache.h"

class CM2Model;

namespace SXC::RetailSheet {
    // Whether a character's body model is a retail one (false without WOWDECOMP_RETAIL_MODELS).
    bool IsRetailBody(const CM2Model* model);

    // Whether the component being composited on this thread is a retail body (set with Scope around the
    // stock section pastes, which are static).
    bool Active();

    class Scope {
        public:
        explicit Scope(bool active);
        ~Scope();

        private:
        bool m_previous;
    };

    // The skin's part for a 3.3.5 section (COMPONENT_SECTIONS) and a layer over it, into the square
    // sheet `sheet` x `sheet` with `levels` mip levels (ARGB8888).
    void PasteSkin(uint32_t section, HTEXTURECACHE texture, MipBits* dst, uint32_t sheet, uint32_t levels);
    void PasteLayer(uint32_t section, HTEXTURECACHE texture, MipBits* dst, uint32_t sheet, uint32_t levels);
}

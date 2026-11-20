// SXC: the body texture of retail character models (WoW/Common/ComponentCore/RetailSheet.h).
#include "WoW/Common/ComponentCore/RetailSheet.h"

#include <algorithm>
#include <cstring>
#include <vector>
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "SXC/Log.h"

#if defined(WOWDECOMP_RETAIL_MODELS)
#include "Engine/Source/Retail/LocalSource.h"
#endif

namespace {
    thread_local bool t_active = false;

    // Retail's rectangles for the 3.3.5 sections, in its 2048 x 1024 sheet (CharComponentTextureSections,
    // layout 2; the 1xx layouts are the same): arms, hands, torso, legs, feet, then both face sections on
    // the right half.
    struct Rect {
        uint32_t x, y, w, h;
    };

    constexpr float kSheetW = 2048.0f, kSheetH = 1024.0f;
    constexpr Rect kRetail[10] = {
        { 0, 0, 512, 256 },        // arm upper
        { 0, 256, 512, 256 },      // arm lower
        { 0, 512, 512, 128 },      // hand
        { 512, 0, 512, 256 },      // torso upper
        { 512, 256, 512, 128 },    // torso lower
        { 512, 384, 512, 256 },    // leg upper
        { 512, 640, 512, 256 },    // leg lower
        { 512, 896, 512, 128 },    // foot
        { 1024, 0, 1024, 1024 },   // head upper: the face
        { 1024, 0, 1024, 1024 },   // head lower: the same face
    };
    constexpr uint32_t kHeadUpper = 8, kHeadLower = 9;

    // --- decoding a BLP mip to B, G, R, A bytes (the sheet's C4Pixel order) -----------------------------
    uint32_t Pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
        return b | g << 8 | r << 16 | a << 24;
    }

    uint32_t From565(uint16_t c, uint32_t a = 255) {
        const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        return Pack(r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, a);
    }

    uint32_t Mix(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t alpha) {
        auto ch = [&](int s) { return (((a >> s) & 255) * wa + ((b >> s) & 255) * wb) / (wa + wb); };
        return Pack(ch(16), ch(8), ch(0), alpha);
    }

    // One DXT colour block into a 4 x 4 of colours; `dxt1` allows the 3-colour mode with transparency.
    void ColourBlock(const uint8_t* p, bool dxt1, uint32_t out[16]) {
        const uint16_t c0 = static_cast<uint16_t>(p[0] | p[1] << 8), c1 = static_cast<uint16_t>(p[2] | p[3] << 8);
        uint32_t colour[4];
        colour[0] = From565(c0);
        colour[1] = From565(c1);

        if (!dxt1 || c0 > c1) {
            colour[2] = Mix(colour[0], colour[1], 2, 1, 255);
            colour[3] = Mix(colour[0], colour[1], 1, 2, 255);
        } else {
            colour[2] = Mix(colour[0], colour[1], 1, 1, 255);
            colour[3] = 0;
        }

        const uint32_t indices = p[4] | p[5] << 8 | p[6] << 16 | static_cast<uint32_t>(p[7]) << 24;

        for (int i = 0; i < 16; i++) {
            out[i] = colour[(indices >> (2 * i)) & 3];
        }
    }

    bool Decode(HTEXTURECACHE texture, uint32_t level, std::vector<uint32_t>& out, uint32_t& w, uint32_t& h) {
        const BLPHeader* header = texture ? texture->m_data : nullptr;
        const uint8_t* bits = static_cast<const uint8_t*>(TextureCacheGetMip(texture, level));

        if (!header || !bits) {
            return false;
        }

        w = std::max(1u, header->m_width >> level);
        h = std::max(1u, header->m_height >> level);
        out.assign(static_cast<size_t>(w) * h, 0);
        const uint32_t pixels = w * h;

        switch (header->m_colorEncoding) {
            case COLOR_PAL: {
                const BlpPalPixel* pal = header->m_extended.m_palette;
                const uint8_t* alpha = bits + pixels;

                for (uint32_t i = 0; i < pixels; i++) {
                    const BlpPalPixel& c = pal[bits[i]];
                    uint32_t a = 255;

                    switch (header->m_alphaSize) {
                        case 1: a = (alpha[i >> 3] >> (i & 7)) & 1 ? 255 : 0; break;
                        case 4: a = ((alpha[i >> 1] >> ((i & 1) * 4)) & 15) * 17; break;
                        case 8: a = alpha[i]; break;
                        default: break;
                    }

                    out[i] = Pack(c.m_r, c.m_g, c.m_b, a);
                }

                return true;
            }

            case COLOR_DXT: {
                // BLP's preferred format: 7 DXT5, 1 DXT3, else DXT1 (with 8-bit alpha: DXT3).
                const int kind = header->m_preferredFormat == 7 ? 5 : (header->m_preferredFormat == 1 || header->m_alphaSize == 8 ? 3 : 1);
                const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, blockBytes = kind == 1 ? 8 : 16;
                uint32_t colour[16];

                for (uint32_t by = 0; by < bh; by++) {
                    for (uint32_t bx = 0; bx < bw; bx++) {
                        const uint8_t* block = bits + (static_cast<size_t>(by) * bw + bx) * blockBytes;
                        uint32_t alpha[16];
                        std::fill(alpha, alpha + 16, 255u);

                        if (kind == 3) {
                            for (int i = 0; i < 16; i++) {
                                alpha[i] = ((block[i >> 1] >> ((i & 1) * 4)) & 15) * 17;
                            }
                        } else if (kind == 5) {
                            const uint32_t a0 = block[0], a1 = block[1];
                            uint32_t table[8] = { a0, a1 };

                            for (int k = 2; k < 8; k++) {
                                table[k] = a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7 : (k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : (k == 6 ? 0 : 255));
                            }

                            uint64_t idx = 0;

                            for (int k = 0; k < 6; k++) {
                                idx |= static_cast<uint64_t>(block[2 + k]) << (8 * k);
                            }

                            for (int i = 0; i < 16; i++) {
                                alpha[i] = table[(idx >> (3 * i)) & 7];
                            }
                        }

                        ColourBlock(kind == 1 ? block : block + 8, kind == 1, colour);

                        for (uint32_t y = 0; y < 4; y++) {
                            for (uint32_t x = 0; x < 4; x++) {
                                const uint32_t px = bx * 4 + x, py = by * 4 + y;

                                if (px < w && py < h) {
                                    const uint32_t c = colour[y * 4 + x];
                                    const uint32_t a = kind == 1 ? (c >> 24) : alpha[y * 4 + x];
                                    out[static_cast<size_t>(py) * w + px] = (c & 0xFFFFFFu) | a << 24;
                                }
                            }
                        }
                    }
                }

                return true;
            }

            case 3:   // uncompressed B, G, R, A
                std::memcpy(out.data(), bits, static_cast<size_t>(pixels) * 4);
                return true;

            default:
                return false;
        }
    }

    // A texture's region (u0, v0)..(u1, v1) scaled into the sheet's rectangle `to` at every mip level, over
    // what's there by the texture's alpha (`blend`) or replacing it.
    void Paste(HTEXTURECACHE texture, MipBits* dst, uint32_t sheet, uint32_t levels, const Rect& to, float u0, float v0, float u1, float v1, bool blend) {
        TCTEXTUREINFO info = {};
        info.m_opaque = 1;

        if (!dst || !TextureCacheHasMips(texture) || !TextureCacheGetInfo(texture, info, 1) || !info.m_width || !info.m_height) {
            return;
        }

        std::vector<uint32_t> src;
        uint32_t srcLevel = ~0u, sw = 0, sh = 0;

        for (uint32_t level = 0; level < levels; level++) {
            const uint32_t size = sheet >> level;

            if (!size) {
                break;
            }

            const uint32_t x0 = to.x >> level, y0 = to.y >> level;
            const uint32_t w = std::max(1u, to.w >> level), h = std::max(1u, to.h >> level);

            // The smallest source mip whose region still covers the target in both directions.
            uint32_t pick = 0;

            while (pick + 1 < info.m_mipCount) {
                const float rw = static_cast<float>(std::max(1u, static_cast<uint32_t>(info.m_width) >> (pick + 1))) * (u1 - u0);
                const float rh = static_cast<float>(std::max(1u, static_cast<uint32_t>(info.m_height) >> (pick + 1))) * (v1 - v0);

                if (rw < static_cast<float>(w) || rh < static_cast<float>(h)) {
                    break;
                }

                pick++;
            }

            if (pick != srcLevel) {
                if (!Decode(texture, pick, src, sw, sh)) {
                    return;
                }

                srcLevel = pick;
            }

            C4Pixel* out = dst->m_mip[level];

            for (uint32_t y = 0; y < h && y0 + y < size; y++) {
                const float v = v0 + (v1 - v0) * (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
                const uint32_t sy = std::min(sh - 1, static_cast<uint32_t>(v * static_cast<float>(sh)));

                for (uint32_t x = 0; x < w && x0 + x < size; x++) {
                    const float u = u0 + (u1 - u0) * (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
                    const uint32_t sx = std::min(sw - 1, static_cast<uint32_t>(u * static_cast<float>(sw)));
                    const uint32_t c = src[static_cast<size_t>(sy) * sw + sx];
                    C4Pixel& p = out[static_cast<size_t>(y0 + y) * size + x0 + x];
                    const uint32_t a = c >> 24;

                    if (!blend) {
                        p.m_b = c & 255;
                        p.m_g = (c >> 8) & 255;
                        p.m_r = (c >> 16) & 255;
                        p.m_a = static_cast<uint8_t>(a);
                    } else if (a) {
                        p.m_b = static_cast<uint8_t>(((c & 255) * a + p.m_b * (255 - a)) / 255);
                        p.m_g = static_cast<uint8_t>((((c >> 8) & 255) * a + p.m_g * (255 - a)) / 255);
                        p.m_r = static_cast<uint8_t>((((c >> 16) & 255) * a + p.m_r * (255 - a)) / 255);
                    }
                }
            }
        }
    }

    // The section's rectangle in the square sheet (retail's layout squeezed sideways).
    Rect InSheet(uint32_t section, uint32_t sheet) {
        const Rect& r = kRetail[section];
        return Rect{ r.x * sheet / 2048, r.y * sheet / 1024, r.w * sheet / 2048, r.h * sheet / 1024 };
    }
}

namespace SXC::RetailSheet {
    bool IsRetailBody(const CM2Model* model) {
        // FlashWOW: retail bodies are drawn by FlashWOW's own customization system (RetailCharacter.cpp: the
        // sheet composed from the Retail customization tables, the items over it, the geosets from the
        // choices), so this sheet stays off for every body. Kept for the shared code paths it guards.
        (void)model;
        return false;
    }

    bool Active() {
        return t_active;
    }

    Scope::Scope(bool active) : m_previous(t_active) {
        t_active = active;
    }

    Scope::~Scope() {
        t_active = m_previous;
    }

    void PasteSkin(uint32_t section, HTEXTURECACHE texture, MipBits* dst, uint32_t sheet, uint32_t levels) {
        // Both face sections are the same square: the skin goes there once.
        if (section >= 10 || section == kHeadLower) {
            return;
        }

        // The skin is retail's whole sheet: the section's own region of it.
        const Rect& r = kRetail[section];
        Paste(texture, dst, sheet, levels, InSheet(section, sheet), r.x / kSheetW, r.y / kSheetH, (r.x + r.w) / kSheetW, (r.y + r.h) / kSheetH, false);
    }

    void PasteLayer(uint32_t section, HTEXTURECACHE texture, MipBits* dst, uint32_t sheet, uint32_t levels) {
        if (section >= 10) {
            return;
        }

        // The face takes square textures only (retail's face, its overlays): 3.3.5's face strips have no
        // place on a retail face and would cover it.
        if (section == kHeadUpper || section == kHeadLower) {
            TCTEXTUREINFO info = {};
            info.m_opaque = 1;

            if (!TextureCacheGetInfo(texture, info, 1) || info.m_width != info.m_height) {
                return;
            }
        }

        Paste(texture, dst, sheet, levels, InSheet(section, sheet), 0.0f, 0.0f, 1.0f, 1.0f, true);
    }
}

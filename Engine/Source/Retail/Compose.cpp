// BLP decoding, BLP building and the character texture compositor, from the Esteria fork of wowdecomp
// (Engine/Source/Retail/Compose.cpp there).
#include "Engine/Source/Retail/Compose.h"
#include <algorithm>
#include <array>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>

namespace Retail {
namespace {

uint32_t U32(const std::vector<uint8_t>& b, size_t p) {
    return uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8) | (uint32_t(b[p + 2]) << 16) | (uint32_t(b[p + 3]) << 24);
}

void Rgb565(uint16_t v, uint8_t out[3]) {
    out[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
    out[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
    out[2] = uint8_t((v & 31) * 255 / 31);
}

// Decodes one 4x4 colour block. `opaque3` selects DXT1's 3-colour + transparent mode.
void ColorBlock(const uint8_t* src, bool dxt1, uint8_t out[16][4]) {
    const uint16_t c0 = uint16_t(src[0] | src[1] << 8), c1 = uint16_t(src[2] | src[3] << 8);
    uint8_t palette[4][4];
    Rgb565(c0, palette[0]); Rgb565(c1, palette[1]);
    palette[0][3] = palette[1][3] = 255;
    if (!dxt1 || c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = uint8_t((2 * palette[0][i] + palette[1][i]) / 3);
            palette[3][i] = uint8_t((palette[0][i] + 2 * palette[1][i]) / 3);
        }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) { palette[2][i] = uint8_t((palette[0][i] + palette[1][i]) / 2); palette[3][i] = 0; }
        palette[2][3] = 255; palette[3][3] = 0;
    }
    const uint32_t bits = uint32_t(src[4]) | uint32_t(src[5]) << 8 | uint32_t(src[6]) << 16 | uint32_t(src[7]) << 24;
    for (int i = 0; i < 16; ++i) std::memcpy(out[i], palette[(bits >> (2 * i)) & 3], 4);
}

void Dxt5Alpha(const uint8_t* src, uint8_t out[16]) {
    uint8_t a[8] = {src[0], src[1]};
    if (a[0] > a[1]) for (int i = 1; i < 7; ++i) a[1 + i] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
    else { for (int i = 1; i < 5; ++i) a[1 + i] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(src[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = a[(bits >> (3 * i)) & 7];
}

}

bool DecodeBlp(const std::vector<uint8_t>& b, Image& out, std::string& error) {
    out = {};
    if (b.size() < 148 || std::memcmp(b.data(), "BLP2", 4) != 0 || U32(b, 4) != 1) { error = "Not a BLP2 texture"; return false; }
    const uint8_t encoding = b[8], alphaDepth = b[9], alphaEncoding = b[10];
    const uint32_t width = U32(b, 12), height = U32(b, 16), offset = U32(b, 20), size = U32(b, 84);
    if (!width || !height || width > 8192 || height > 8192 || offset > b.size() || size > b.size() - offset) {
        error = "Invalid BLP dimensions or mip table"; return false;
    }
    out.width = width; out.height = height;
    out.rgba.assign(size_t(width) * height * 4, 0);
    const uint8_t* data = b.data() + offset;
    if (encoding == 3) {
        if (size < size_t(width) * height * 4) { error = "Truncated BLP pixels"; return false; }
        for (size_t i = 0; i < size_t(width) * height; ++i) {
            out.rgba[i * 4 + 0] = data[i * 4 + 2]; out.rgba[i * 4 + 1] = data[i * 4 + 1];
            out.rgba[i * 4 + 2] = data[i * 4 + 0]; out.rgba[i * 4 + 3] = data[i * 4 + 3];
        }
        return true;
    }
    if (encoding == 1) {
        const size_t pixels = size_t(width) * height;
        const size_t alphaBytes = alphaDepth == 8 ? pixels : alphaDepth == 1 ? (pixels + 7) / 8 : alphaDepth == 4 ? (pixels + 1) / 2 : 0;
        if (size < pixels + alphaBytes) { error = "Truncated BLP palette image"; return false; }
        for (size_t i = 0; i < pixels; ++i) {
            const uint32_t color = U32(b, 148 + size_t(data[i]) * 4);
            out.rgba[i * 4 + 0] = uint8_t(color >> 16); out.rgba[i * 4 + 1] = uint8_t(color >> 8); out.rgba[i * 4 + 2] = uint8_t(color);
            uint8_t alpha = 255;
            if (alphaDepth == 8) alpha = data[pixels + i];
            else if (alphaDepth == 1) alpha = (data[pixels + i / 8] >> (i % 8)) & 1 ? 255 : 0;
            else if (alphaDepth == 4) alpha = uint8_t(((data[pixels + i / 2] >> (4 * (i % 2))) & 15) * 17);
            out.rgba[i * 4 + 3] = alpha;
        }
        return true;
    }
    if (encoding != 2) { error = "Unsupported BLP encoding"; return false; }
    const bool dxt1 = alphaDepth == 0 || alphaEncoding == 0;
    const bool dxt3 = !dxt1 && alphaEncoding == 1;
    const uint32_t blockBytes = dxt1 ? 8 : 16;
    const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (size < size_t(bw) * bh * blockBytes) { error = "Truncated BLP DXT data"; return false; }
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* block = data + (size_t(by) * bw + bx) * blockBytes;
            uint8_t colors[16][4], alpha[16];
            ColorBlock(dxt1 ? block : block + 8, dxt1, colors);
            if (dxt1) for (int i = 0; i < 16; ++i) alpha[i] = colors[i][3];
            else if (dxt3) for (int i = 0; i < 16; ++i) alpha[i] = uint8_t(((block[i / 2] >> (4 * (i % 2))) & 15) * 17);
            else Dxt5Alpha(block, alpha);
            for (int i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x >= width || y >= height) continue;
                uint8_t* pixel = &out.rgba[(size_t(y) * width + x) * 4];
                pixel[0] = colors[i][0]; pixel[1] = colors[i][1]; pixel[2] = colors[i][2]; pixel[3] = alpha[i];
            }
        }
    }
    return true;
}

void BuildBlp(const Image& image, std::vector<uint8_t>& blp) {
    // Header, then every mip level as BGRA. Each level is the 2x2 box average of the one above (edge
    // texels repeat on odd sizes). The whole file is sized up front and each level is written straight
    // into it; the averaging runs in row bands on several threads for the large levels.
    std::vector<std::pair<uint32_t, uint32_t>> sizes;
    for (uint32_t width = image.width, height = image.height;;) {
        sizes.emplace_back(width, height);
        if ((width == 1 && height == 1) || sizes.size() == 16) break;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    size_t total = 1172;
    for (const auto& size : sizes) total += size_t(size.first) * size.second * 4;
    blp.assign(total, 0);
    std::memcpy(blp.data(), "BLP2", 4);
    auto put = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) blp[at + i] = uint8_t(v >> (8 * i)); };
    put(4, 1);
    blp[8] = 3; blp[9] = 8; blp[10] = 8; blp[11] = 1; // raw BGRA, 8-bit alpha, preferred format 8 below, mips present
    put(12, image.width); put(16, image.height);

    std::vector<uint8_t> level = image.rgba, next;
    size_t offset = 1172;
    for (size_t mip = 0; mip < sizes.size(); ++mip) {
        const uint32_t width = sizes[mip].first, height = sizes[mip].second;
        const size_t bytes = size_t(width) * height * 4;
        put(20 + mip * 4, uint32_t(offset));
        put(84 + mip * 4, uint32_t(bytes));
        uint8_t* out = blp.data() + offset;
        for (size_t i = 0; i < bytes; i += 4) {
            out[i] = level[i + 2]; out[i + 1] = level[i + 1]; out[i + 2] = level[i]; out[i + 3] = level[i + 3];
        }
        offset += bytes;
        if (mip + 1 == sizes.size()) break;

        const uint32_t nextWidth = sizes[mip + 1].first, nextHeight = sizes[mip + 1].second;
        next.assign(size_t(nextWidth) * nextHeight * 4, 0);
        auto rows = [&, width, height, nextWidth](uint32_t first, uint32_t end) {
            for (uint32_t y = first; y < end; ++y) {
                const uint8_t* r0 = level.data() + size_t(std::min(y * 2, height - 1)) * width * 4;
                const uint8_t* r1 = level.data() + size_t(std::min(y * 2 + 1, height - 1)) * width * 4;
                uint8_t* o = next.data() + size_t(y) * nextWidth * 4;
                for (uint32_t x = 0; x < nextWidth; ++x) {
                    const size_t c0 = size_t(std::min(x * 2, width - 1)) * 4, c1 = size_t(std::min(x * 2 + 1, width - 1)) * 4;
                    for (uint32_t c = 0; c < 4; ++c)
                        o[x * 4 + c] = uint8_t((uint32_t(r0[c0 + c]) + r0[c1 + c] + r1[c0 + c] + r1[c1 + c]) / 4);
                }
            }
        };
        const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
        const uint32_t bands = uint64_t(nextWidth) * nextHeight < 64 * 1024 ? 1 : std::min({hardware, 8u, nextHeight});
        std::vector<std::thread> threads;
        const uint32_t per = (nextHeight + bands - 1) / bands;
        for (uint32_t band = 1; band < bands; ++band) {
            const uint32_t first = band * per, end = std::min(nextHeight, first + per);
            if (first < end) threads.emplace_back(rows, first, end);
        }
        rows(0, std::min(nextHeight, per));
        for (auto& thread : threads) thread.join();
        level.swap(next);
    }
}

namespace {

struct Rect { uint32_t x, y, w, h; };

// Stretches `src` over `rect` of `dst` with bilinear sampling and the given blend.
//
// The arithmetic is the reference painter's, operation for operation, so the result is identical to
// the byte; only the bookkeeping is cheaper: the horizontal sample positions are worked out once per
// column instead of once per pixel, n/255 comes from a table holding the same float values, source
// texels with zero alpha are skipped where the blend leaves the destination unchanged, and large
// rectangles are split into row bands painted on several threads (rows are independent).
struct Column { int x0, x1; float tx; };

void PaintRows(Image& dst, const Image& src, const Rect& rect, uint32_t blend, bool legacyAlphaOver,
    const std::vector<Column>& columns, const float* unit, uint32_t firstRow, uint32_t endRow) {
    auto byte = [](float value) { return uint8_t(std::min(255.0f, std::max(0.0f, value)) + 0.5f); };
    // Blends whose result equals the destination when the source alpha is zero: alpha-over, and the
    // compositor modes (fa = 0 leaves RGB and alpha as they were).
    const bool skipClear = blend != 0 && blend != 1;
    const uint8_t* const texels = src.rgba.data();
    const size_t stride = size_t(src.width) * 4;
    for (uint32_t y = firstRow; y < endRow; ++y) {
        const float fy = (float(y) + 0.5f) * src.height / rect.h - 0.5f;
        const int y0 = std::max(0, int(std::floor(fy))), y1 = std::min(int(src.height) - 1, y0 + 1);
        const float ty = std::min(1.0f, std::max(0.0f, fy - float(y0)));
        const uint8_t* const row0 = texels + size_t(y0) * stride;
        const uint8_t* const row1 = texels + size_t(y1) * stride;
        uint8_t* d = &dst.rgba[(size_t(rect.y + y) * dst.width + rect.x) * 4];
        for (uint32_t x = 0; x < rect.w; ++x, d += 4) {
            const Column& column = columns[x];
            const uint8_t* const a0 = row0 + size_t(column.x0) * 4;
            const uint8_t* const a1 = row0 + size_t(column.x1) * 4;
            const uint8_t* const b0 = row1 + size_t(column.x0) * 4;
            const uint8_t* const b1 = row1 + size_t(column.x1) * 4;
            const float tx = column.tx;
            float s[4];
            for (int c = 0; c < 4; ++c) {
                const float top = a0[c] * (1 - tx) + a1[c] * tx;
                const float bottom = b0[c] * (1 - tx) + b1[c] * tx;
                s[c] = top * (1 - ty) + bottom * ty;
            }
            // Straight alpha-over onto a fully transparent texel zeroes its RGB, so that case still paints.
            if (skipClear && s[3] == 0.0f && (!legacyAlphaOver || d[3] != 0)) continue;
            const float a = s[3] / 255.0f, da = unit[d[3]];
            if (blend == 0 || blend == 1) { // DB2 blit preserves RGB even under zero alpha.
                for (int c = 0; c < 4; ++c) d[c] = byte(s[c]);
            } else if (legacyAlphaOver) { // Legacy equipment PaintImage callers use straight alpha-over.
                const float oa = a + da * (1 - a);
                for (int c = 0; c < 3; ++c)
                    d[c] = byte(oa > 0 ? (s[c] * a + d[c] * da * (1 - a)) / oa : 0);
                d[3] = byte(oa * 255.0f);
            } else {
                // ChrModelTextureLayer uses the character compositor's enum, not M2 material
                // blend modes. Match wow.export char.fragment.shader + CharMaterialRenderer at
                // c2fd7bde36a712be78a5da896c995b84fbfa2545: shader result, then GPU blending.
                // In particular, opaque white tattoo texels multiply to unchanged skin.
                const float fa = blend == 4 ? da * a : a;
                for (int c = 0; c < 3; ++c) {
                    const float base = unit[d[c]], src = s[c] / 255.0f;
                    float result = src;
                    if (blend == 4) result = base * src; // Multiply
                    else if (blend == 6) // Overlay, thresholded by the source.
                        result = src < 0.5f ? 2 * base * src : 1 - 2 * (1 - base) * (1 - src);
                    else if (blend == 7) result = 1 - (1 - base) * (1 - src); // Screen
                    d[c] = byte((result * fa + base * (1 - fa)) * 255.0f);
                }
                // Mode 9 has separate ONE/INVSRCALPHA alpha factors. The other modes
                // use SRCALPHA/INVSRCALPHA for alpha as well as RGB (including mode 15).
                d[3] = byte((blend == 9 ? fa + da * (1 - fa) : fa * fa + da * (1 - fa)) * 255.0f);
            }
        }
    }
}

void Paint(Image& dst, const Image& src, const Rect& rect, uint32_t blend, bool legacyAlphaOver = false) {
    if (!rect.w || !rect.h || rect.x + rect.w > dst.width || rect.y + rect.h > dst.height) return;
    if (!src.width || !src.height || src.rgba.size() < size_t(src.width) * src.height * 4) return;
    static const auto unit = [] {
        std::array<float, 256> table{};
        for (int i = 0; i < 256; ++i) table[i] = i / 255.0f;
        return table;
    }();
    std::vector<Column> columns(rect.w);
    for (uint32_t x = 0; x < rect.w; ++x) {
        const float fx = (float(x) + 0.5f) * src.width / rect.w - 0.5f;
        const int x0 = std::max(0, int(std::floor(fx))), x1 = std::min(int(src.width) - 1, x0 + 1);
        columns[x] = {x0, x1, std::min(1.0f, std::max(0.0f, fx - float(x0)))};
    }
    const uint64_t pixels = uint64_t(rect.w) * rect.h;
    const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t bands = pixels < 128 * 1024 ? 1 : std::min({hardware, 8u, rect.h});
    if (bands <= 1) {
        PaintRows(dst, src, rect, blend, legacyAlphaOver, columns, unit.data(), 0, rect.h);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(bands - 1);
    const uint32_t per = (rect.h + bands - 1) / bands;
    for (uint32_t band = 1; band < bands; ++band) {
        const uint32_t first = band * per, end = std::min(rect.h, first + per);
        if (first >= end) break;
        threads.emplace_back(PaintRows, std::ref(dst), std::cref(src), std::cref(rect), blend, legacyAlphaOver,
            std::cref(columns), unit.data(), first, end);
    }
    PaintRows(dst, src, rect, blend, legacyAlphaOver, columns, unit.data(), 0, std::min(rect.h, per));
    for (auto& thread : threads) thread.join();
}

}

void PaintImage(Image& dst, const Image& src, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t blend) {
    Paint(dst, src, {x, y, width, height}, blend, blend == 2);
}

bool ComposeTextures(uint32_t chrModel, const std::vector<Appearance::TextureLayer>& layers, BlpFetch fetch, void* context,
    std::vector<ComposedTexture>& out, std::string& warnings, ComposeStats* stats) {
    out.clear();
    using Clock = std::chrono::steady_clock;
    const auto since = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    const uint32_t layout = Appearance::LayoutFor(chrModel);
    uint32_t sheetWidth = 0, sheetHeight = 0;
    const bool haveSheet = Appearance::LayoutSize(layout, sheetWidth, sheetHeight);
    std::vector<Appearance::TextureSection> sections;
    Appearance::Sections(layout, sections);
    std::map<uint32_t, ComposedTexture> byType;
    for (const Appearance::TextureLayer& layer : layers) { // already in paint order
        std::vector<uint8_t> bytes;
        Image image;
        std::string error;
        auto start = Clock::now();
        const bool fetched = layer.fileDataId && fetch(layer.fileDataId, bytes, context);
        if (stats) { stats->fetch += since(start); ++stats->files; }
        start = Clock::now();
        const bool decoded = fetched && DecodeBlp(bytes, image, error);
        if (stats) stats->decode += since(start);
        if (!decoded) {
            warnings += "texture " + std::to_string(layer.fileDataId) + " unavailable" + (error.empty() ? "" : ": " + error) + "\n";
            continue;
        }
        start = Clock::now();
        struct PaintTimer {
            ComposeStats* stats; Clock::time_point start;
            ~PaintTimer() { if (stats) stats->paint += std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
        } paintTimer{stats, start};
        ComposedTexture& target = byType[layer.textureType];
        target.textureType = layer.textureType;
        if (!target.image.width) {
            // The body sheet has the layout's size; every other type takes the size of its first layer.
            const bool body = layer.textureType == 1 && haveSheet;
            target.image.width = body ? sheetWidth : image.width;
            target.image.height = body ? sheetHeight : image.height;
            target.image.rgba.assign(size_t(target.image.width) * target.image.height * 4, 0);
        }
        const int32_t mask = int32_t(layer.sectionMask);
        if (mask == -1 || layer.textureType != 1) {
            Paint(target.image, image, {0, 0, target.image.width, target.image.height}, layer.blendMode);
            continue;
        }
        for (const Appearance::TextureSection& section : sections)
            if (section.sectionType < 32 && (uint32_t(mask) >> section.sectionType & 1))
                Paint(target.image, image, {section.x, section.y, section.width, section.height}, layer.blendMode);
    }
    for (auto& entry : byType) out.push_back(std::move(entry.second));
    return !out.empty();
}

}

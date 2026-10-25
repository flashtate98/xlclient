// sxc.fog (SXC/Api/sxc_fog.h) and its seam in the world render (SXC/Fog/Fog.h).

#include "SXC/Fog/Fog.h"

#include <intrin.h>
#include <immintrin.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>
#include "SXC/Api/sxc_fog.h"
#include "Engine/Source/Os/TimeManager.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"

namespace {
    using namespace SXC::Fog;

    Renderer s_renderer = nullptr;
    StatusFn s_status = nullptr;

    sxc_fog_look s_look = {};
    bool s_haveLook = false;

    // The field, as the renderer takes it (half floats), and its window.
    std::vector<uint16_t> s_light, s_pool, s_tint;
    bool s_haveTint = false;
    bool s_haveReady = false; // the pool layer's w holds how much the near field shows
    uint32_t s_size = 0;
    float s_cellSize = 2.0f;
    int32_t s_firstI = 0, s_firstJ = 0;
    uint32_t s_version = 0;

    // The far map and the wake, the same way.
    std::vector<uint16_t> s_farLayer, s_farTint;
    bool s_haveFarTint = false;
    uint32_t s_farSize = 0;
    float s_farCellSize = 33.333f;
    int32_t s_farFirstI = 0, s_farFirstJ = 0;
    uint32_t s_farVersion = 0;

    std::vector<uint16_t> s_wake;
    uint32_t s_wakeSize = 0;
    float s_wakeTexel = 1.0f;
    float s_wakeOrigin[2] = {};
    uint32_t s_wakeVersion = 0;

    // How far the patches have drifted (yd): the look's drift velocity summed frame by frame, so a
    // changing wind turns the drift instead of jumping the whole pattern.
    double s_drift[2] = {};
    uint32_t s_lastMs = 0;

    // IEEE half from float (round to nearest; overflow to infinity, underflow to zero).
    uint16_t Half(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        const uint32_t sign = (bits >> 16) & 0x8000u;
        const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = bits & 0x7FFFFFu;

        if (((bits >> 23) & 0xFF) == 0xFF) {
            return static_cast<uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
        }

        if (exponent >= 31) {
            return static_cast<uint16_t>(sign | 0x7C00u);
        }

        if (exponent <= 0) {
            if (exponent < -10) {
                return static_cast<uint16_t>(sign);
            }

            mantissa |= 0x800000u;
            const uint32_t shift = static_cast<uint32_t>(14 - exponent);
            uint32_t half = mantissa >> shift;

            if ((mantissa >> (shift - 1)) & 1u) {
                half++;
            }

            return static_cast<uint16_t>(sign | half);
        }

        uint32_t half = sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);

        if (mantissa & 0x1000u) {
            half++; // may carry into the exponent: still the right rounding
        }

        return static_cast<uint16_t>(half);
    }

    // F16C (every x64 CPU since 2012; asked once): four values an instruction.
    bool HasF16c() {
        static const int s_has = [] {
            int regs[4] = {};
            __cpuid(regs, 1);
            return (regs[2] >> 29) & 1;
        }();
        return s_has != 0;
    }

    void Convert(const float* src, std::vector<uint16_t>& dst, size_t count) {
        const size_t n = count * 4;
        dst.resize(n);

        if (!src) {
            std::fill(dst.begin(), dst.end(), uint16_t(0));
            return;
        }

        if (HasF16c()) {
            const __m128 zero = _mm_setzero_ps();

            for (size_t i = 0; i < n; i += 4) {
                const __m128 v = _mm_loadu_ps(src + i);
                // Not finite (x - x isn't 0 for infinities and NaN): 0, as below.
                const __m128 finite = _mm_cmpeq_ps(_mm_sub_ps(v, v), zero);
                const __m128i h = _mm_cvtps_ph(_mm_and_ps(v, finite), _MM_FROUND_TO_NEAREST_INT);
                _mm_storel_epi64(reinterpret_cast<__m128i*>(dst.data() + i), h);
            }

            return;
        }

        for (size_t i = 0; i < n; i++) {
            dst[i] = Half(std::isfinite(src[i]) ? src[i] : 0.0f);
        }
    }

    // --- the service --------------------------------------------------------------------------------

    wxl_bool WXL_CALL Available() {
        return s_renderer ? WXL_TRUE : WXL_FALSE;
    }

    void WXL_CALL SetField(const sxc_fog_field* field) {
        if (!field || field->struct_size < offsetof(sxc_fog_field, colour) || !field->light || field->size == 0 || field->size > SXC_FOG_MAX_FIELD) {
            s_size = 0;
            s_light.clear();
            s_pool.clear();
            s_tint.clear();
            s_haveTint = false;
            s_version++;
            return;
        }

        const size_t cells = static_cast<size_t>(field->size) * field->size;
        Convert(field->light, s_light, cells);
        Convert(field->pool, s_pool, cells); // zeros without pools
        // The colour per cell (appended to the struct): without it, the look's colour everywhere.
        const float* tint = field->struct_size >= offsetof(sxc_fog_field, colour) + sizeof(field->colour) ? field->colour : nullptr;
        s_haveTint = tint != nullptr;

        if (s_haveTint) {
            Convert(tint, s_tint, cells);
        } else {
            s_tint.clear();
        }

        // How much the near field shows per cell (appended later): into the pool layer's spare channel.
        const float* ready = field->struct_size >= offsetof(sxc_fog_field, ready) + sizeof(field->ready) ? field->ready : nullptr;
        s_haveReady = ready != nullptr;

        if (s_haveReady) {
            for (size_t i = 0; i < cells; i++) {
                const float r = std::isfinite(ready[i]) ? ready[i] : 1.0f;
                s_pool[i * 4 + 3] = Half(r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r));
            }
        }

        s_size = field->size;
        s_cellSize = field->cell_size > 0.01f ? field->cell_size : 0.01f;
        s_firstI = field->first_i;
        s_firstJ = field->first_j;
        s_version++;
    }

    void WXL_CALL SetLook(const sxc_fog_look* look) {
        if (!look || look->struct_size < offsetof(sxc_fog_look, step_length)) {
            s_haveLook = false;
            return;
        }

        s_look = sxc_fog_look{};
        std::memcpy(&s_look, look, look->struct_size < sizeof(sxc_fog_look) ? look->struct_size : sizeof(sxc_fog_look));
        s_haveLook = true;
    }

    const char* WXL_CALL Status() {
        if (!s_renderer) {
            return "off: this renderer doesn't draw volumetric fog (Vulkan only)";
        }

        if (!s_haveLook || !s_look.enabled) {
            return "off: no mod turned it on";
        }

        return s_status ? s_status() : "";
    }

    void WXL_CALL StockColour(float* out) {
        if (!out) {
            return;
        }

        const CImVector& stock = DayNight::GetInfo()->m_fog.m_color;
        out[0] = stock.r / 255.0f;
        out[1] = stock.g / 255.0f;
        out[2] = stock.b / 255.0f;
    }

    void WXL_CALL SetFar(const sxc_fog_far* map) {
        s_farVersion++;

        if (!map || map->struct_size < sizeof(sxc_fog_far) || !map->layer || map->size == 0 || map->size > SXC_FOG_MAX_FAR) {
            s_farSize = 0;
            s_farLayer.clear();
            s_farTint.clear();
            s_haveFarTint = false;
            return;
        }

        const size_t cells = static_cast<size_t>(map->size) * map->size;
        Convert(map->layer, s_farLayer, cells);
        s_haveFarTint = map->colour != nullptr;

        if (s_haveFarTint) {
            Convert(map->colour, s_farTint, cells);
        } else {
            s_farTint.clear();
        }

        s_farSize = map->size;
        s_farCellSize = map->cell_size > 0.01f ? map->cell_size : 0.01f;
        s_farFirstI = map->first_i;
        s_farFirstJ = map->first_j;
    }

    void WXL_CALL SetWake(const sxc_fog_wake* wake) {
        s_wakeVersion++;

        if (!wake || wake->struct_size < sizeof(sxc_fog_wake) || !wake->texels || wake->size < 2 || wake->size > SXC_FOG_MAX_WAKE) {
            s_wakeSize = 0;
            s_wake.clear();
            return;
        }

        Convert(wake->texels, s_wake, static_cast<size_t>(wake->size) * wake->size);
        s_wakeSize = wake->size;
        s_wakeTexel = wake->texel > 0.01f ? wake->texel : 0.01f;
        s_wakeOrigin[0] = wake->origin[0];
        s_wakeOrigin[1] = wake->origin[1];
    }

    const sxc_fog_api s_api = { sizeof(sxc_fog_api), Available, SetField, SetLook, Status, StockColour, SetFar, SetWake };
}

namespace SXC::Fog {
    const sxc_fog_api* Api() {
        return &s_api;
    }

    void SetRenderer(Renderer renderer, StatusFn status) {
        s_renderer = renderer;
        s_status = status;
        s_version++; // a new device holds no field yet
        s_farVersion++;
        s_wakeVersion++;
    }

    bool WaterWritesDepth() {
        return s_renderer && s_haveLook && s_look.enabled && (s_size > 0 || s_farSize > 0);
    }

    void Composite() {
        const CGWorldFrame* frame = CGWorldFrame::s_currentWorldFrame;

        if (!s_renderer || !s_haveLook || !s_look.enabled || !frame || !frame->m_camera) {
            return;
        }

        Input in;
        const C3Vector& camera = frame->m_camera->m_position;
        in.origin[0] = camera.x;
        in.origin[1] = camera.y;
        in.origin[2] = camera.z;
        const uint32_t now = OsGetAsyncTimeMs();
        const float dt = s_lastMs ? static_cast<float>(now - s_lastMs) * 0.001f : 0.0f;
        s_lastMs = now;
        in.seconds = static_cast<float>(now % 3600000u) * 0.001f;

        if (dt > 0.0f && dt < 1.0f) {
            for (int k = 0; k < 2; k++) {
                s_drift[k] = std::fmod(s_drift[k] + static_cast<double>(s_look.drift[k]) * dt, 100000.0);
            }
        }

        // The colour: the place's stock fog colour (what the sky's distance fog has), or the mod's.
        float colour[3] = { s_look.colour[0], s_look.colour[1], s_look.colour[2] };

        if (s_look.colour_mode == 0) {
            const CImVector& stock = DayNight::GetInfo()->m_fog.m_color;
            colour[0] = stock.r / 255.0f;
            colour[1] = stock.g / 255.0f;
            colour[2] = stock.b / 255.0f;
        }

        for (int k = 0; k < 3; k++) {
            in.colour[k] = colour[k] * s_look.brightness;
        }

        if (s_size) {
            in.fieldSize = s_size;
            in.cellSize = s_cellSize;
            in.firstI = s_firstI;
            in.firstJ = s_firstJ;
            in.light = s_light.data();
            in.pool = s_pool.data();
            in.tint = s_haveTint ? s_tint.data() : nullptr;
            in.tintBrightness = s_look.brightness;
            in.fieldReady = s_haveReady;
        }

        in.fieldVersion = s_version;

        if (s_farSize) {
            in.farSize = s_farSize;
            in.farCellSize = s_farCellSize;
            in.farFirstI = s_farFirstI;
            in.farFirstJ = s_farFirstJ;
            in.farLayer = s_farLayer.data();
            in.farTint = s_haveFarTint ? s_farTint.data() : nullptr;
        }

        in.farVersion = s_farVersion;

        if (s_wakeSize) {
            in.wakeSize = s_wakeSize;
            in.wakeTexel = s_wakeTexel;
            in.wakeOrigin[0] = s_wakeOrigin[0];
            in.wakeOrigin[1] = s_wakeOrigin[1];
            in.wake = s_wake.data();
        }

        in.wakeVersion = s_wakeVersion;
        in.densityScale = s_look.density_scale;
        in.noiseAmount = s_look.noise_amount;
        in.noiseTile = s_look.noise_tile;
        in.drift[0] = static_cast<float>(s_drift[0]);
        in.drift[1] = static_cast<float>(s_drift[1]);
        in.farDistance = s_look.far_distance > 10.0f ? s_look.far_distance : 10.0f;
        in.hazeDensity = s_look.haze_density;
        in.hazeBase = s_look.haze_base;
        in.hazeThickness = s_look.haze_thickness;
        in.debug = s_look.debug;
        in.stepLength = s_look.step_length > 0.0f ? s_look.step_length : 0.5f;
        in.perPixel = s_look.method == 0;
        in.maxSteps = s_look.max_steps > 0 ? s_look.max_steps : 160;
        in.handover = s_look.handover > 2.0f ? s_look.handover : 16.0f; // zero in a shorter (older) look
        s_renderer(in);
    }
}

// The Vulkan backend: construction, the device lifecycle (create, set format, destroy), the gamma
// ramp and the crash-log lines. The window is CGxDeviceVulkanWindow.cpp, the caps
// CGxDeviceVulkanCaps.cpp, the frame CGxDeviceVulkanScene.cpp, the pools CGxDeviceVulkanPool.cpp,
// the textures CGxDeviceVulkanTexture.cpp, the shaders CGxDeviceVulkanShader.cpp, the draw
// CGxDeviceVulkanDraw.cpp, the render targets CGxDeviceVulkanTarget.cpp, the queries
// CGxDeviceVulkanQuery.cpp, the cursor CGxDeviceVulkanCursor.cpp, the rest CGxDeviceVulkanStubs.cpp.
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Fog.hpp"  // SXC: volumetric fog
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Grass.hpp"  // SXC: the instanced grass
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Manifest.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Manifest.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Present.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Queries.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Shaders.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Timings.hpp"  // SXC: the frame profiler's GPU times
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Error.h"
#include "Engine/Source/Storm/H/storm.h"
#include "SXC/Fog/Fog.h"  // SXC: volumetric fog
#include "SXC/Cover/Cover.h"  // SXC: surface covers
#include "SXC/Lights/Lights.h" // SXC: point lights
#include "SXC/Log.h"
#include "SXC/Profiler/Profiler.h"  // SXC: the frame profiler

namespace context   = gxvk::vk::context;
namespace frame     = gxvk::vk::frame;
namespace manifest  = gxvk::vk::manifest;
namespace memory    = gxvk::vk::memory;
namespace pipelines = gxvk::vk::pipelines;
namespace present   = gxvk::vk::present;
namespace queries   = gxvk::vk::queries;
namespace shaders   = gxvk::vk::shaders;
namespace texture   = gxvk::vk::texture;

// Mirrors CreateCGxDeviceD3d (0x0068E0B0) and CreateCGxDeviceOpenGl (0x0068BF20): the engine's
// allocator, placement new.
CGxDevice* CreateCGxDeviceVulkan() {
    void* mem = SMemAlloc(sizeof(CGxDeviceVulkan), __FILE__, __LINE__, 0);

    if (!mem) {
        return nullptr;
    }

    return new (mem) CGxDeviceVulkan();
}

// Mirrors CGxDeviceD3d::CGxDeviceD3d (0x0068DE70): the API tag, the colour order the engine swizzles
// vertex colours to (D3D's ARGB, which is what the B8G8R8A8 swapchain and the D3DCOLOR vertex fetch
// expect), the stream pools.
CGxDeviceVulkan::CGxDeviceVulkan() {
    this->m_api = GxApi_Vulkan;
    this->m_hwnd = nullptr;
    this->m_hwndClass = 0;
    this->m_ownhwnd = 0;
    this->m_vulkanPending = 0;
    this->m_drawPath = 0;
    this->m_filling = nullptr;
    this->m_manifestTried = 0;
    this->m_hcursor = nullptr;
    this->m_hwCursorDirty = 0;
    // D3D's constant registers start at zero.
    memset(this->m_vertexRegs, 0, sizeof(this->m_vertexRegs));
    memset(this->m_pixelRegs, 0, sizeof(this->m_pixelRegs));
    this->m_caps.m_colorFormat = GxCF_argb;
    this->m_caps.unk_0018 = 1;
    this->DeviceCreatePools();
    this->DeviceCreateStreamBufs();
}

CGxDeviceVulkan::~CGxDeviceVulkan() {
}

// Mirrors CGxDeviceD3d::DeviceCreate (0x00690750): the system gamma ramp, the window class, then the
// base DeviceCreate, which stores the engine's window proc and calls DeviceSetFormat.
int32_t CGxDeviceVulkan::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    this->m_ownhwnd = 1;

    HDC dc = GetDC(nullptr);

    if (GetDeviceGammaRamp(dc, &this->m_systemGammaRamp) && &this->m_gammaRamp != &this->m_systemGammaRamp) {
        this->m_gammaRamp = this->m_systemGammaRamp;
    }

    ReleaseDC(nullptr, dc);

    WNDCLASSEXA windowClass;
    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = CGxDeviceVulkan::WindowProcVulkan;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = "GxWindowClassVulkan";
    windowClass.hIcon = static_cast<HICON>(LoadImageA(windowClass.hInstance, "BlizzardIcon.ico", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    windowClass.hCursor = LoadCursorA(windowClass.hInstance, "BlizzardCursor.cur");

    if (!windowClass.hCursor) {
        windowClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    }

    this->m_hwndClass = RegisterClassExA(&windowClass);

    if (!this->m_hwndClass) {
        char* error = OsGetLastErrorStr();
        CGxDevice::Log("CGxDeviceVulkan::DeviceCreate(): RegisterClassExA() failed: %s", error);
        OsFreeLastErrorStr(error);
        this->DeviceDestroy();
        return 0;
    }

    if (CGxDevice::DeviceCreate(windowProc, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

// Mirrors CGxDeviceD3d::DeviceCreate (0x00690830): a window the caller owns.
int32_t CGxDeviceVulkan::DeviceCreate(void* window, const CGxFormat& format) {
    this->m_ownhwnd = 0;
    CGxDevice::DeviceCreate(window, format);
    this->m_hwnd = static_cast<HWND>(window);

    if (this->ICreateVulkan(format) && CGxDevice::DeviceCreate(window, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

// Mirrors CGxDeviceD3d::DeviceDestroy (0x006905F0).
void CGxDeviceVulkan::DeviceDestroy() {
    CGxDevice::DeviceDestroy();
    this->IDestroyVulkan();

    if (this->m_hwnd && this->m_ownhwnd) {
        DestroyWindow(this->m_hwnd);
        this->m_hwnd = nullptr;
    }

    if (this->m_hwndClass) {
        UnregisterClassA(reinterpret_cast<const char*>(static_cast<uintptr_t>(this->m_hwndClass)), GetModuleHandleA(nullptr));
        this->m_hwndClass = 0;
    }
}

// Mirrors CGxDeviceD3d::DeviceSetFormat (0x006904D0): a new window for the new format. The Vulkan
// instance and device survive the window, and with them the pools' buffers and the pipelines; the
// surface and the swapchain are rebuilt on the new window.
int32_t CGxDeviceVulkan::DeviceSetFormat(const CGxFormat& format) {
    CGxDevice::Log("CGxDeviceVulkan::DeviceSetFormat():");
    this->Log(format);

    if (this->m_hwnd) {
        ShowWindow(this->m_hwnd, SW_HIDE);
    }

    this->IReleaseVulkanWindow();
    this->IDestroyWindow();
    this->m_format = format;

    CGxFormat createFormat = format;

    if (this->ICreateWindow(createFormat) && this->ICreateVulkan(createFormat) && CGxDevice::DeviceSetFormat(createFormat)) {
        this->ISetWindowFocus(1);
        return 1;
    }

    CGxDevice::Log("CGxDeviceVulkan::DeviceSetFormat(): unable to set format!");
    this->IReleaseVulkanWindow();
    this->IDestroyWindow();

    return 0;
}

// The Vulkan half of CGxDeviceD3d::ICreateD3dDevice (0x0068F3D0): the context on the window, the
// draw path, then the caps, the defaults, the gamma ramp and the cursor, as D3D does once its device
// exists.
int32_t CGxDeviceVulkan::ICreateVulkan(const CGxFormat& format) {
    // Validation is a developer switch: GX_VULKAN_VALIDATION=1 in the environment asks for the
    // Khronos layer; a layer that is not installed is a log line, not a failure.
    char validation[8];
    DWORD length = GetEnvironmentVariableA("GX_VULKAN_VALIDATION", validation, sizeof(validation));
    context::SetValidation(length > 0 && length < sizeof(validation) && validation[0] != '0');

    // Before the swapchain is built: the format's vsync picks its present mode.
    context::SetVsync(format.m_vsync != 0);

    this->m_vulkanPending = 0;

    if (context::Live()) {
        // A device that is up and a window that is new: the format changed, or the engine restarted
        // its graphics. Rebind sees the HWND differ and rebuilds surface and swapchain.
        context::Rebind(this->m_hwnd);
        this->ICreateDrawPath();
    } else {
        switch (context::Bring(this->m_hwnd)) {
            case context::Attempt::Declined:
                CGxDevice::Log("CGxDeviceVulkan::ICreateVulkan(): the Vulkan device could not be brought up (the lines above say why)");
                return 0;

            case context::Attempt::Retry:
                // The window is not measurable yet (minimised): the device comes up on the first
                // present that finds it. The caps below are the table's defaults until then.
                CGxDevice::Log("CGxDeviceVulkan::ICreateVulkan(): the window is not measurable yet; the Vulkan device comes up at the first present");
                this->m_vulkanPending = 1;
                break;

            default:
                this->ICreateDrawPath();
                break;
        }
    }

    this->m_context = 1;
    this->ISetCaps(format);
    this->Log(this->m_caps);

    this->IStateSetDefaults();
    this->DeviceSetGamma(this->m_gammaRamp);
    this->ICursorCreate(format);

    return 1;
}

// Device memory, the textures, the translated shaders' manifest and modules, the pipelines, the
// present pass and the queries, once the VkDevice exists. Without them the pools answer locks with
// the engine's emergency memory, the textures stay pending and nothing reaches the window. The
// manifest is looked for once per device: without it only the backend's own programs draw.
void CGxDeviceVulkan::ICreateDrawPath() {
    if (this->m_drawPath || !context::Live()) {
        return;
    }

    char folder[512];
    this->IShaderFolder(folder, sizeof(folder));

    if (!this->m_manifestTried) {
        this->m_manifestTried = 1;
        manifest::Load(folder);
    }

    this->m_drawPath = memory::Init() && texture::Init() && shaders::Init(folder) && pipelines::Init() && present::Init() && queries::Init();

    if (!this->m_drawPath) {
        CGxDevice::Log("CGxDeviceVulkan::ICreateDrawPath(): device memory, the textures, the shaders, the pipelines, the present pass or the queries could not be brought up (the lines above say why); nothing is drawn");
    } else {
        SXC::Fog::SetRenderer(&CGxDeviceVulkan::SxcFogRender, &gxvk::vk::fog::Status);  // SXC: volumetric fog
        gxvk::vk::cover::SetProvider(&CGxDeviceVulkan::SxcCoverFrame);                  // SXC: surface covers
        SXC::Cover::SetDevice(gxvk::vk::cover::Ready(), gxvk::vk::cover::TextureSlots(), &gxvk::vk::cover::Status);
        gxvk::vk::lights::SetProvider(&CGxDeviceVulkan::SxcLightsFrame);                // SXC: point lights
        gxvk::vk::lights::SetShadowReport(&SXC::Lights::ShadowsDrawn);
        SXC::Lights::SetDevice(gxvk::vk::cover::Ready() && gxvk::vk::lights::Ready(), &gxvk::vk::lights::Status);
        SXC::Lights::SetShadowDevice(gxvk::vk::shadows::Ready() ? gxvk::vk::shadows::Slots() : 0, gxvk::vk::shadows::kMaxVertices);

        // SXC: the instanced grass.
        static const SXC::Grass::Device s_grass = { gxvk::vk::grass::Mesh, gxvk::vk::grass::Allocate, gxvk::vk::grass::Write, gxvk::vk::grass::Release,
                                                    &CGxDeviceVulkan::SxcGrassDraw, gxvk::vk::grass::PlantsInUse };
        SXC::Grass::SetDevice(gxvk::vk::grass::Init() ? &s_grass : nullptr);

        // SXC: the frame profiler's GPU times (the pre-pass kinds into the profiler's pre-pass rows).
        static_assert(SXC::Profiler::kMaxCategories <= gxvk::vk::timings::kMaxCategories, "the timings carry every profiler category");

        if (gxvk::vk::timings::Init()) {
            gxvk::vk::timings::SetSink([](const double* main, const double* pre) {
                double ms[SXC::Profiler::kMaxCategories];
                memcpy(ms, main, sizeof(ms));
                ms[SXC::Profiler::kUploads] += pre[static_cast<int>(gxvk::vk::timings::Pre::Uploads)];
                ms[SXC::Profiler::kShadowMaps] += pre[static_cast<int>(gxvk::vk::timings::Pre::ShadowMaps)];
                SXC::Profiler::GpuTimes(ms, SXC::Profiler::kMaxCategories);
            });
            static const SXC::Profiler::Gpu s_gpu = { gxvk::vk::timings::Mark, gxvk::vk::timings::Enable, gxvk::vk::timings::Status };
            SXC::Profiler::SetGpu(&s_gpu);
        }
    }
}

// SXC: the instanced grass (SXC/Grass): a texture's runs with the client's grass state as it stands --
// the DetailDoodad shaders and constants CDetailDoodad set, the texture at stage 0 -- but the vertex
// module's instanced variant (manifest `instanced`), the plant models' buffers and the plant pool fed
// per instance (rows at locations 13..15, the normal and colour as the stock vertex's).
bool CGxDeviceVulkan::SxcGrassDraw(CGxTex* texture, const SXC::Grass::Run* runs, uint32_t count) {
    CGxDeviceVulkan* device = static_cast<CGxDeviceVulkan*>(g_theGxDevicePtr);

    if (!device || !device->m_context || device->m_intF5C || !device->m_drawPath || !texture || !runs || !count || !gxvk::vk::grass::Ready()) {
        return false;
    }

    device->RsSet(GxRs_Texture0, texture);
    device->IRsSync(0);

    if (!gxvk::vk::frame::Pass()) {
        return false;
    }

    gxvk::vk::pipelines::Draw d;
    d.streamCount = 3;
    d.streams[0] = { gxvk::vk::grass::MeshVertices(), 0, gxvk::vk::grass::kVertexBytes };
    d.streams[1] = { gxvk::vk::grass::Plants(), 0, gxvk::vk::grass::kPlantBytes };
    d.streams[2] = { gxvk::vk::grass::Copies(), 0, 0 };   // step 7d: the copy number, at stride 0 (each run binds its entry)
    d.attribs[GxVA_Position] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT };
    d.attribs[GxVA_TexCoord0] = { 0, 12, VK_FORMAT_R32G32_SFLOAT };
    d.attribs[GxVA_Normal] = { 1, 48, VK_FORMAT_R32G32B32_SFLOAT };
    d.attribs[GxVA_Color0] = { 1, 60, VK_FORMAT_B8G8R8A8_UNORM };
    d.attribMask = (1u << GxVA_Position) | (1u << GxVA_TexCoord0) | (1u << GxVA_Normal) | (1u << GxVA_Color0);
    d.instanceStreams = (1u << 1) | (1u << 2);
    d.plantStream = 1;
    d.copyStream = 2;
    d.indexed = true;
    d.indexBuffer = gxvk::vk::grass::MeshIndices();
    d.topology = GxPrim_Triangles;
    d.count = 1;

    if (!device->IDrawState(d)) {
        return false;
    }

    // The bound DetailDoodad vertex record's instanced variant; without one the stock way is all there is.
    const gxvk::vk::manifest::Record* instanced = d.vertex == gxvk::vk::pipelines::VertexProgram::Translated && d.vertexRecord
                                                      ? gxvk::vk::manifest::Instanced(*d.vertexRecord)
                                                      : nullptr;

    if (!instanced) {
        SXC::LogOnce("grass-no-variant", "grass: the bound grass vertex shader has no instanced variant (re-run the shader translator)");
        return false;
    }

    d.vertexRecord = instanced;
    d.lightsSurface = gxvk::vk::lights::kSurfaceOutdoor;   // step 7c: grass grows outside (our lights' inside / outside test)
    d.vertexRows = instanced->vconst == gxvk::vk::pipelines::kVertexRowsLarge ? gxvk::vk::pipelines::kVertexRowsLarge : gxvk::vk::pipelines::kVertexRowsSmall;
    static_assert(sizeof(SXC::Grass::Run) == sizeof(gxvk::vk::pipelines::Run), "SXC::Grass::Run is the pipelines' Run");
    d.runs = reinterpret_cast<const gxvk::vk::pipelines::Run*>(runs);
    d.runCount = count;

    VkCommandBuffer cmd = gxvk::vk::frame::Pass();

    if (!cmd || !gxvk::vk::pipelines::Record(cmd, gxvk::vk::frame::RenderPass(), gxvk::vk::frame::PassGeneration(), gxvk::vk::frame::Serial(),
                                             gxvk::vk::frame::Slot(), d)) {
        return false;
    }

    // The frame profiler's draw count: a draw per run.
    if (SXC::Profiler::On()) {
        for (uint32_t i = 0; i < count; i++) {
            SXC::Profiler::CountDraw(runs[i].indexCount / 3 * runs[i].instanceCount);
        }
    }

    return true;
}

// SXC: our point lights, gathered once per frame at its first terrain draw (SXC/Lights).
bool CGxDeviceVulkan::SxcLightsFrame(gxvk::vk::lights::Frame& out) {
    CGxDeviceVulkan* device = static_cast<CGxDeviceVulkan*>(g_theGxDevicePtr);

    if (!device || !device->m_drawPath) {
        return false;
    }

    const SXC::Lights::Frame& in = SXC::Lights::Current();

    if (!in.on) {
        return false;
    }

    out.on = true;
    out.debug = in.debug;
    out.originX = in.originX;
    out.originY = in.originY;
    out.cellSize = in.cellSize;
    out.cells = in.cells;
    out.perCell = in.perCell;
    out.lightCount = in.lightCount;
    out.lights = in.lights;
    out.cellLights = in.cellLights;
    std::memcpy(out.viewInv, in.viewInv, sizeof out.viewInv);

    // The jobs, their alpha-keyed parts' textures resolved to this device's images (a texture not on the
    // GPU yet: the part is left out and the slot looked at again soon).
    static std::vector<gxvk::vk::shadows::Job> jobs;
    static std::vector<gxvk::vk::shadows::Part> parts;
    jobs.resize(in.shadowJobCount);
    parts.clear();

    size_t partTotal = 0;   // reserved whole: the jobs point into it

    for (uint32_t i = 0; i < in.shadowJobCount; i++) {
        partTotal += in.shadowJobs[i].partCount;
    }

    parts.reserve(partTotal);

    for (uint32_t i = 0; i < in.shadowJobCount; i++) {
        const SXC::Lights::ShadowJob& job = in.shadowJobs[i];
        jobs[i].slot = job.slot;
        jobs[i].radius = job.radius;
        jobs[i].vertices = job.vertices;
        jobs[i].vertexCount = job.vertexCount;
        jobs[i].parts = nullptr;
        jobs[i].partCount = job.partCount;
        bool incomplete = false;
        const size_t first = parts.size();

        for (uint32_t p = 0; p < job.partCount; p++) {
            gxvk::vk::shadows::Part part;
            part.firstVertex = job.parts[p].firstVertex;
            part.vertexCount = job.parts[p].vertexCount;

            if (job.parts[p].texture) {
                part.texture = device->SxcShadowTexture(job.parts[p].texture);

                if (part.texture == VK_NULL_HANDLE) {
                    part.vertexCount = 0;
                    incomplete = true;
                }
            }

            parts.push_back(part);
        }

        jobs[i].parts = job.partCount ? parts.data() + first : nullptr;

        if (incomplete) {
            SXC::Lights::ShadowIncomplete(job.slot);
        }
    }

    out.shadowJobs = jobs.empty() ? nullptr : jobs.data();
    out.shadowJobCount = in.shadowJobCount;
    return true;
}

// SXC: the covers' data for our terrain shader, once per frame at its first terrain draw: what the mod
// set (SXC/Cover), the cover textures resolved to this device's images (and stamped with this frame).
bool CGxDeviceVulkan::SxcCoverFrame(gxvk::vk::cover::Frame& out) {
    SXC::Profiler::Scope profile(SXC::Profiler::kCoverFrame);
    CGxDeviceVulkan* device = static_cast<CGxDeviceVulkan*>(g_theGxDevicePtr);
    const SXC::Cover::Frame& in = SXC::Cover::Current();

    if (!device || !device->m_drawPath || !in.on) {
        return false;
    }

    out.on = true;
    out.fallenSnow = in.fallenSnow;
    out.debug = in.debug;
    out.brightness = in.brightness;
    out.wetDarkening = in.wetDarkening;
    out.thinDepth = in.thinDepth;
    out.depthScale = in.depthScale;
    out.breakupScale = in.breakupScale;
    out.driftScale = in.driftScale;
    out.textureTile = in.textureTile;
    std::memcpy(out.outside, in.outside, sizeof out.outside);
    out.tessDetail = in.tessDetail;
    out.tessReach = in.tessReach;
    out.tessMost = in.tessMost;
    out.trails = in.trails;
    out.trailNow = in.trailNow;
    out.trailPersist = in.trailPersist;
    out.trailSlur = in.trailSlur;
    out.trailRim = in.trailRim;
    const SXC::Cover::Frame::TrailLayer* trailIn[2] = { &in.trailFine, &in.trailCoarse };
    gxvk::vk::cover::Frame::TrailLayer* trailOut[2] = { &out.trailFine, &out.trailCoarse };

    for (int k = 0; k < 2; k++) {
        trailOut[k]->size = trailIn[k]->size;
        trailOut[k]->cell = trailIn[k]->cell;
        trailOut[k]->firstI = trailIn[k]->firstI;
        trailOut[k]->firstJ = trailIn[k]->firstJ;
        trailOut[k]->cells = trailIn[k]->cells;
        trailOut[k]->rects = trailIn[k]->rects;
        trailOut[k]->rectCount = trailIn[k]->rectCount;
        trailOut[k]->full = trailIn[k]->full;
    }
    out.surface = in.surface;
    out.surfaceSize = in.surfaceSize;
    out.surfaceCell = in.surfaceCell;
    out.surfaceFirstI = in.surfaceFirstI;
    out.surfaceFirstJ = in.surfaceFirstJ;
    out.surfaceSink = [](const float* cells, uint32_t size, float cell) { SXC::Cover::TakeSurface(cells, size, cell); };
    out.entries = in.entries;
    out.entryCount = in.entryCount;
    out.nearSize = in.nearSize;
    out.nearCell = in.nearCell;
    out.nearFirstI = in.nearFirstI;
    out.nearFirstJ = in.nearFirstJ;
    out.nearCells = in.nearCells;
    out.nearVersion = in.nearVersion;
    out.farSize = in.farSize;
    out.farCell = in.farCell;
    out.farFirstI = in.farFirstI;
    out.farFirstJ = in.farFirstJ;
    out.farCells = in.farCells;
    out.farVersion = in.farVersion;
    out.textureGloss = in.textureGloss;
    out.slotCount = in.slotCount < gxvk::vk::cover::kBindlessSlots ? in.slotCount : gxvk::vk::cover::kBindlessSlots;
    static pipelines::Draw scratch;  // ITexResolve writes a draw's stage; one is reused

    for (uint32_t s = 0; s < out.slotCount; s++) {
        out.slotTexture[s] = in.slotTexture[s];
        scratch.textureMask = 0;

        if (in.slotGx[s] && device->ITexResolve(in.slotGx[s], 0, scratch) && !scratch.textures[0].cube) {
            out.slots[s] = scratch.textures[0];
        }
    }

    return true;
}

// SXC: the volumetric fog over the current target (SXC/Fog/Fog.h), with this device's own view of the
// draws: the view and native projection they're transformed with, the viewport they land in.
bool CGxDeviceVulkan::SxcFogRender(const SXC::Fog::Input& in) {
    CGxDeviceVulkan* device = static_cast<CGxDeviceVulkan*>(g_theGxDevicePtr);

    if (!device || !device->m_drawPath) {
        return false;
    }

    gxvk::vk::fog::Frame f;
    VkRect2D scissor = {};

    if (!device->IPlaceD3d(f.viewport, scissor)) {
        return false;
    }

    const CGxMatrixStack& view = device->m_xforms[GxXform_View];
    const C44Matrix viewProj = view.m_mtx[view.m_level] * device->m_projNative;
    std::memcpy(f.viewProj, &viewProj, sizeof f.viewProj);
    std::memcpy(f.origin, in.origin, sizeof f.origin);
    f.seconds = in.seconds;
    std::memcpy(f.colour, in.colour, sizeof f.colour);
    f.fieldSize = in.fieldSize;
    f.cellSize = in.cellSize;
    f.firstI = in.firstI;
    f.firstJ = in.firstJ;
    f.light = in.light;
    f.pool = in.pool;
    f.tint = in.tint;
    f.tintBrightness = in.tintBrightness;
    f.fieldVersion = in.fieldVersion;
    f.fieldReady = in.fieldReady;
    f.farSize = in.farSize;
    f.farCellSize = in.farCellSize;
    f.farFirstI = in.farFirstI;
    f.farFirstJ = in.farFirstJ;
    f.farLayer = in.farLayer;
    f.farTint = in.farTint;
    f.farVersion = in.farVersion;
    f.wakeSize = in.wakeSize;
    f.wakeTexel = in.wakeTexel;
    f.wakeOrigin[0] = in.wakeOrigin[0];
    f.wakeOrigin[1] = in.wakeOrigin[1];
    f.wake = in.wake;
    f.wakeVersion = in.wakeVersion;
    f.densityScale = in.densityScale;
    f.noiseAmount = in.noiseAmount;
    f.noiseTile = in.noiseTile;
    f.drift[0] = in.drift[0];
    f.drift[1] = in.drift[1];
    f.nearDistance = in.nearDistance;
    f.farDistance = in.farDistance;
    f.hazeDensity = in.hazeDensity;
    f.hazeBase = in.hazeBase;
    f.hazeThickness = in.hazeThickness;
    f.debug = in.debug;
    f.stepLength = in.stepLength;
    f.perPixel = in.perPixel;
    f.maxSteps = in.maxSteps;
    f.handover = in.handover;
    f.lightStrength = in.lightStrength;
    f.lightGlow = in.lightGlow;
    f.lightRange = in.lightRange;
    return gxvk::vk::fog::Record(f);
}

// A bring-up that answered Retry at DeviceSetFormat: tried again from ScenePresent while the window
// is visible, and the caps refined from the device once it exists.
void CGxDeviceVulkan::IBringUpIfPending() {
    if (!this->m_vulkanPending || !this->IWindowVisible()) {
        return;
    }

    switch (context::Bring(this->m_hwnd)) {
        case context::Attempt::Done:
            this->m_vulkanPending = 0;
            this->ICreateDrawPath();
            this->ISetCaps(this->m_format);
            this->Log(this->m_caps);
            break;

        case context::Attempt::Declined:
            this->m_vulkanPending = 0;
            this->m_context = 0;
            CGxDevice::Log("CGxDeviceVulkan::IBringUpIfPending(): the Vulkan device could not be brought up; the context is lost");
            break;

        default:
            break;
    }
}

// Before the window goes (a format change): the frame's swapchain objects, then the surface. The
// instance, the VkDevice, the pools' buffers and the pipelines stay for the next window.
void CGxDeviceVulkan::IReleaseVulkanWindow() {
    frame::ReleaseSwapchain();
    context::ReleaseWindow();
    this->m_context = 0;
}

// Everything, in reverse order of creation: the frame's swapchain objects (which waits for the
// device, and drops the present pass's framebuffers), the pools' buffers and the textures' images,
// the frame's own objects (the retired ones with them), the pipelines, the present pass, the
// queries, the shader modules, the textures' table, device memory, the context, the manifest.
void CGxDeviceVulkan::IDestroyVulkan() {
    this->ICursorDestroy();
    SXC::Fog::SetRenderer(nullptr, nullptr);  // SXC: volumetric fog
    SXC::Cover::SetDevice(false, 0, nullptr); // SXC: surface covers
    SXC::Grass::SetDevice(nullptr);           // SXC: the instanced grass (its buffers go below)
    SXC::Profiler::SetGpu(nullptr);           // SXC: the frame profiler's GPU times (the pools go below)
    frame::ReleaseSwapchain();
    this->IReleasePools();
    this->IReleaseTextures();
    gxvk::vk::fog::Shutdown();  // SXC: the device is idle (ReleaseSwapchain waited)
    gxvk::vk::grass::Shutdown();
    frame::Destroy();
    pipelines::Shutdown();
    present::Shutdown();
    queries::Shutdown();
    gxvk::vk::timings::Shutdown();  // SXC:
    shaders::Shutdown();
    texture::Shutdown();
    memory::Shutdown();
    this->m_drawPath = 0;
    this->m_filling = nullptr;
    context::LogValidationSummary();
    context::Teardown();
    manifest::Unload();
    this->m_manifestTried = 0;
    this->m_context = 0;
    this->m_vulkanPending = 0;
}

// The engine half of CGxDeviceD3d::IStateSetD3DDefaults (0x006A3A60): every render state marked for
// re-send, the vertex streams and the index buffer dirty, the lights reset, the shader constants
// cleared.
void CGxDeviceVulkan::IStateSetDefaults() {
    this->IRsForceUpdate();
    this->IRsSync(0);
    this->m_primVertexDirty = 0xFFFFFFFF;
    this->m_primIndexDirty = 0;
    this->InitLights();
    this->ShaderConstantsClear();
}

void* CGxDeviceVulkan::DeviceWindow() {
    return this->m_hwnd;
}

// Mirrors CGxDeviceD3d::DeviceSetGamma (0x0068E4C0 / 0x0069FE80): the base keeps the ramp; the
// present pass applies it to the frame it shows, in fullscreen only, as D3D's SetGammaRamp applies
// to a fullscreen swap chain alone.
void CGxDeviceVulkan::DeviceSetGamma(float gamma) {
    CGxDevice::DeviceSetGamma(gamma);
    present::SetGamma(this->m_gammaRamp.m_red, this->m_gammaRamp.m_green, this->m_gammaRamp.m_blue, !this->IDevIsWindowed());
}

void CGxDeviceVulkan::DeviceSetGamma(const CGxGammaRamp& ramp) {
    CGxDevice::DeviceSetGamma(ramp);
    present::SetGamma(this->m_gammaRamp.m_red, this->m_gammaRamp.m_green, this->m_gammaRamp.m_blue, !this->IDevIsWindowed());
}

// Mirrors CGxDeviceD3d::DeviceOverride (0x0069FF40) for the one override the shader level depends on.
void CGxDeviceVulkan::DeviceOverride(EGxOverride override, uint32_t value) {
    CGxDevice::DeviceOverride(override, value);

    if (override == GxOverride_PixelShader) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = value;
    }
}

void CGxDeviceVulkan::LogCrashInfo(char* buffer, uint32_t size) {
    char name[256];
    char driver[128];
    context::AdapterText(name, sizeof(name), driver, sizeof(driver));

    char line[512];

    if (name[0]) {
        SStrPrintf(line, sizeof(line), "Adapter: %s\r\n", name);
        SStrPack(buffer, line, size);
        SStrPrintf(line, sizeof(line), "Driver: %s\r\n", driver);
        SStrPack(buffer, line, size);
    } else {
        SStrPack(buffer, "Adapter: no Vulkan device\r\n", size);
    }

    frame::Stats f;
    frame::GetStats(f);
    pipelines::Stats p;
    pipelines::GetStats(p);
    texture::Stats t;
    texture::GetStats(t);
    SStrPrintf(line, sizeof(line), "Frames: %llu submitted, %llu presented, %llu passes (%llu resumed, %llu on render targets); draws: %llu drawn, %llu skipped, %u pipeline(s), %u sampler(s)\r\n",
               static_cast<unsigned long long>(f.frames), static_cast<unsigned long long>(f.presented),
               static_cast<unsigned long long>(f.passes), static_cast<unsigned long long>(f.passResumes),
               static_cast<unsigned long long>(f.targetPasses),
               static_cast<unsigned long long>(p.drawn), static_cast<unsigned long long>(p.skipped), p.pipelines, p.samplers);
    SStrPack(buffer, line, size);

    queries::Stats q;
    queries::GetStats(q);
    SStrPrintf(line, sizeof(line), "Targets: %u depth buffer(s), %u framebuffer(s), %llu scene copies, %llu scene reads; queries: %llu begun, %llu ended, %llu read (%llu waited, %llu flushed), %llu cut by a pass end\r\n",
               f.depthBuffers, f.framebuffers, static_cast<unsigned long long>(f.sceneCopies), static_cast<unsigned long long>(f.sceneReads),
               static_cast<unsigned long long>(q.begun), static_cast<unsigned long long>(q.ended), static_cast<unsigned long long>(q.results),
               static_cast<unsigned long long>(q.waits), static_cast<unsigned long long>(q.flushes), static_cast<unsigned long long>(f.queriesCut));
    SStrPack(buffer, line, size);
    SStrPrintf(line, sizeof(line), "Textures: %u image(s) live (peak %u), %llu region(s) uploaded in %llu batch(es), %llu MiB staged\r\n",
               t.live, t.livePeak, static_cast<unsigned long long>(t.regions), static_cast<unsigned long long>(t.batches),
               static_cast<unsigned long long>(f.stagedBytes >> 20));
    SStrPack(buffer, line, size);

    manifest::Stats m;
    manifest::GetStats(m);
    shaders::Stats sh;
    shaders::GetStats(sh);
    SStrPrintf(line, sizeof(line), "Shaders: manifest %s (%u record(s)), %u translated module(s) loaded, %u failed; %llu uniform slice(s), %llu key(s) refused\r\n",
               manifest::Loaded() ? "loaded" : "absent", m.records, sh.loaded, sh.failed,
               static_cast<unsigned long long>(p.regsSlices), static_cast<unsigned long long>(p.refusedKeys));
    SStrPack(buffer, line, size);
}

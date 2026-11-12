// SXC: a second view of the world (kb: second-camera). Ours: what a view is - a camera (here: the player's,
// mirrored at the water's height, with the water as its near plane), its target, its users - and the
// choice of when one is drawn. The device draws it (Vulkan/View.cpp): while the world's opaque part is
// drawn once, as always, each draw is copied into the view's target through the view's projection. The
// stock world render isn't run twice; what the player's camera drew is what the view has.
//
// Its first user is the water's mirror (Seyri, 2026-11-04: still lakes showed what the screen-space
// reflections can't - bridges' and piers' undersides, skyboxes off screen, leaves haloed with sky, the
// player's own model).
#pragma once

#include <cstdint>

class C3Vector;

namespace SXC::View {
    // What the device offers (the Vulkan device registers it; none: no second views). `proj` the frame's
    // projection and `view` the view's in its place (both row-major, row vectors, as the device's ProjNative),
    // the target the screen's size divided by `divisor`.
    // The view as transforms of view space (`mirror` the mirrored camera's, `plane` the water there, its near
    // plane; `sky` the sky's, mirrored through the camera): the device makes each draw's projection the view's.
    // `proj` the frame's projection.
    struct Device {
        bool (*begin)(const float mirror[16], const float plane[4], const float sky[16], const float proj[16], uint32_t divisor, bool grass, float grassReach);
        void (*end)();
        void (*stats)(uint32_t& copied, uint32_t& left, uint32_t& noProjection, uint32_t& width, uint32_t& height);
        void (*sky)(bool on);
    };

    void SetDevice(const Device* device);

    // The world's opaque part begins (CWorldScene::Render, after the scene's clear) and ends (CGWorldFrame,
    // before the liquid pass): the mirror is recorded in between, when there is water to mirror.
    void WorldBegin();
    void WorldEnd();

    // Whether this frame mirrors, and at which plane (world z), decided before the world is culled
    // (CWorldScene::Update), so its culling takes the mirror's frustum too; WorldBegin keeps to it.
    // `camera` the world position the world is drawn from.
    bool PlanMirror(const C3Vector& camera, float& planeZ);

    // How far the mirror's culling reaches (yd): what it adds lies within this of the camera (huge: as far
    // as the view).
    float MirrorReach();

    // The plan this frame's culling took: mirroring at `planeZ`, `full` as far as the view (the distant
    // terrain's culling takes its mirror too).
    bool Planned(float& planeZ, bool& full);

    // The sky is drawn (DayNight::RenderSky, on and off): its dome and skyboxes are small models around the
    // camera that the device pushes behind everything, so the mirror takes them as directions - mirrored
    // through the camera, not at the water (at the water, the dome was a ring below the player, shrinking
    // as the camera rose: Seyri, 2026-11-05).
    void Sky(bool on);

    // The mirror this frame: drawn, at this plane (world z).
    bool Mirror(float& planeZ);

    // The Water panel's part: the switch, the size, what was copied.
    void DrawPanel();
}

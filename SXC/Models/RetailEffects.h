#pragma once

// SXC: parts of retail's model shaders that the stock shaders lack, done in the translated model pixel
// modules (Tools/VulkanShaders, emitter.py) and switched per draw through the draw's misc row.
//
// Edge fade (retail's Diffuse_EdgeFade_* vertex shaders, packed ids 0x13, 0x15, 0x19, 33): the alpha
// times clamp(2.7 d^2 - 0.4, 0, 1), d = the view direction against the normal, so surfaces seen edge-on
// fade out (soft cloud and dust shells instead of bubbles). Retail fades per vertex; ours per pixel,
// from the view position and normal the vertex modules hand on (sxcView, sxcNormal).
//
// Not yet: retail's depth fade (Combiners_Mod_Depth's soft intersection with the scene).
namespace SXC::Models {

    // CM2SceneRender::SetupTextures, per batch: the draws that follow fade their edges (or not).
    void SetDrawEdgeFade(bool on);

    // The device, for a draw: misc.y of the translated modules (1 = fade the edges).
    float DrawEdgeFade();

}

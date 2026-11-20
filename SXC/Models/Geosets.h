#pragma once

class CM2Model;

// The Geosets panel (Models module, kb: modern-models): every geoset of the player's or the target's
// model by group, which are shown, and switches that override the client's choice. For finding out which
// geosets a body has and which of them the 3.3.5 rules get wrong (retail bodies' feet, chests, helmets).
// Overrides hold per model file for the session and are re-applied after the client sets a character's
// geosets, so they survive gear changes.
namespace SXC::Geosets {
    void DrawPanel(void* user);

    // The Batches panel: each batch of the same model - geoset, shader id, blend, the effect the client
    // chose and the texture bound on each stage - for finding why a model draws wrong.
    void DrawBatchesPanel(void* user);

    // The session's overrides for this model's file, applied (CCharacterComponent::GeosRenderPrep).
    void ApplyOverrides(CM2Model* model);
}

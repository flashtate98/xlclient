// SXC: how our water's surface moves at a world point, shared by Water.vert (which moves its vertices with it)
// and WaterQuery.comp (which tells the CPU where the surface is under each swimmer, so they ride the waves:
// SXC/Water/Ride.cpp). One function, so what a swimmer rides is exactly what is drawn.
//
// Needs WaterScene.glsl, WaterShore.glsl and the waves' displacement (`waveDisp`, set 2 binding 4) before it.
//
// The stock mesh has a vertex about every 4 yd: it carries the long waves (cascade 0, smoothed, the sideways
// part small, capped, fading with distance); the short ones stay in the shading. How far: the wave height and
// this water's look (its movement and wave scale).

const float kMoveHalfWorld = 32.0 * 533.33333;

// The look's movement row (Water.vert's vLook[4]: glint sharpness, wave scale, foam, movement) of terrain
// water at a point, from the look grid between the four nearest chunk centres; `fallback` without a grid.
vec4 LookMoveAt(vec3 world, vec4 fallback)
{
    if (scene.lookGrid.z < 0.5)
    {
        return fallback;
    }

    const float chunk = 533.33333 / 16.0;
    vec2 g = vec2(kMoveHalfWorld - world.y, kMoveHalfWorld - world.x) / chunk - scene.lookGrid.xy - 0.5;
    g = clamp(g, vec2(0.0), vec2(63.0));
    ivec2 i0 = min(ivec2(g), ivec2(62));
    vec2 f = g - vec2(i0);
    ivec2 at[4] = ivec2[4](i0, i0 + ivec2(1, 0), i0 + ivec2(0, 1), i0 + ivec2(1, 1));
    vec4 row[4];

    for (int n = 0; n < 4; n++)
    {
        int b = at[n].y * 64 + at[n].x;
        uint word = scene.lookIndex[b >> 4][(b >> 2) & 3];
        row[n] = scene.lookPalette[((word >> uint((b & 3) * 8)) & 0xFFu) * 5u + 4u];
    }

    return mix(mix(row[0], row[1], f.x), mix(row[2], row[3], f.x), f.y);
}

// The surface's movement at `world` (yd): `nearMesh` and `nearCell` for the near mesh (its cell: surface,
// ground, type, covered), `depthY` the stock mesh's depth coordinate, `look` the movement row above, `camera`
// the camera's world position, `time` the water's clock (s). `ease`: how freely it moves here (the debug view).
vec3 SurfaceMove(vec3 world, bool nearMesh, vec4 nearCell, float depthY, vec4 look, vec3 camera, float time, out float ease)
{
    // How freely the surface moves here: eased out toward the shore. By the shore field where it reaches
    // (still at the waterline and in water under a yard or so deep, free some 8 yd out); beyond it by the
    // vertex's own depth (the MH2O depth byte through the depth table: 0 at the waterline, 1 a few yards
    // deep). Moved at the beach, the water's edge rose above the sand and showed its steps (Seyri,
    // 2026-10-29). WMO liquid carries neither, so it stays still.
    float shoreWeight;
    vec4 shore = ShoreAt(world.xy, shoreWeight);
    float deep = mix(smoothstep(0.05, 0.6, depthY), smoothstep(0.5, 8.0, shore.x) * smoothstep(0.3, 2.0, shore.y), shoreWeight);
    float sideways = deep;

    // The near mesh reaches under the land, so its waves run on to the waterline and the ground cuts them
    // there (eased like the stock mesh, they went still before the sand: Seyri, 2026-10-30). A little calmer
    // in the shallows; sideways only out of them, so the mesh's edges stay over their cells.
    if (nearMesh)
    {
        float depth = nearCell.x - nearCell.y;
        deep = mix(0.6, 1.0, smoothstep(0.0, 4.0, depth));
        sideways = smoothstep(0.5, 3.0, depth);
    }

    ease = deep;
    vec3 move = vec3(0.0);

    // The swash on the near mesh: the water near the waterline rises a little as each wave arrives and sinks
    // as it slides back, so the waterline itself runs up the beach and back (a flat beach far, a steep one
    // hardly: the ground decides where the raised surface meets it).
    if (nearMesh && shoreWeight > 0.0)
    {
        float exposure = ShoreExposure(world.xy, shore);
        float runUp = scene.shoreLook.y * 1.5 * exposure * look.y * (1.0 - smoothstep(1.0, 6.0, abs(shore.x)));
        move.z += runUp * ShoreSwash(ShoreArrival(world.xy, time)) * shoreWeight;
    }

    if (scene.waveSizes.w > 0.5 && look.w > 0.0)
    {
        // Only what the mesh can hold, so the surface doesn't tear (Seyri, 2026-10-29: far-out waves at height
        // and movement 1 grew choppy, then split apart): cascade 0 alone, averaged over the vertex spacing
        // (shorter waves alias into spikes on a 4 yd mesh); the sideways part small (at full chop neighbouring
        // vertices passed each other and the triangles turned over); capped; and fading out with distance,
        // where the shading carries the waves alone.
        float distance = length(world - camera);
        float cascade0 = 1.0 / scene.waveSizes.x;
        const float kSpacing = 2.0;   // yd, half the stock mesh's
        vec3 d = (textureLod(waveDisp, vec3(world.xy * cascade0, 0.0), 0.0).xyz * 2.0
                + textureLod(waveDisp, vec3((world.xy + vec2(kSpacing, 0.0)) * cascade0, 0.0), 0.0).xyz
                + textureLod(waveDisp, vec3((world.xy - vec2(kSpacing, 0.0)) * cascade0, 0.0), 0.0).xyz
                + textureLod(waveDisp, vec3((world.xy + vec2(0.0, kSpacing)) * cascade0, 0.0), 0.0).xyz
                + textureLod(waveDisp, vec3((world.xy - vec2(0.0, kSpacing)) * cascade0, 0.0), 0.0).xyz) / 6.0;
        d.xy *= 0.3;
        d = clamp(d, vec3(-1.0), vec3(1.0));
        d *= 1.0 - smoothstep(150.0, 260.0, distance);
        // Eased out toward the shore (`deep`, above): the coast stays put and joins, open water moves fully.
        float scale = look.w * look.y;   // this water's movement and wave scale
        move.xy += d.xy * (scale * sideways);
        move.z += d.z * (scale * deep);
    }

    // Land the near mesh reaches under: the waves and the run-up may lift it over the ground near the
    // waterline, but not at the mesh's outer edge, which would stand out of the ground.
    if (nearMesh && nearCell.x < nearCell.y)
    {
        float cap = nearCell.y - 0.05 - world.z + 100.0 * (1.0 - smoothstep(2.0, 4.5, -shore.x));
        move.z = min(move.z, cap);
    }

    return move;
}

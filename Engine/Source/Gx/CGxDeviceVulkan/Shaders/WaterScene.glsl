// SXC: our water's set 2 parameters (Vulkan/Water.cpp's Params, std140), shared by Water.vert and
// Water.frag: where the draws lie on the target and the target's size, the waves, the depth conversion's
// terms (the debug view's self-test), the sky's bodies.
layout(std140, set = 2, binding = 2) uniform WaterScene {
    vec4 viewport;    // the draws' viewport on the target (x, y, width, height)
    vec4 extent;      // the target (width, height, 1 / width, 1 / height)
    vec4 waveSizes;   // the cascades' world sizes (yd); w 1 when the waves run
    vec4 waveLook;    // how far cascades 1 and 2 reach (yd), the foam's strength, the surface's movement
    vec4 depthProj;   // the capture's projection terms (P22, P32, P23, P33) and
    vec4 depthRange;  // depth range (min, max): the debug view's self-test; w how many sky bodies follow
    vec4 bodyDir[4];  // the sky's bodies as drawn this frame: direction toward each, angular radius (rad)
    vec4 bodyColour[4];
} scene;

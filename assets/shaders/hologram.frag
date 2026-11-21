#version 460
// Sample game material: animated scanlines and a fresnel rim on top of the engine's lighting.
// Material::customParams: rgb = hologram color, a = scanline density (0 = 18 lines per unit).
// Edit and save this file while the editor runs: it is recompiled and swapped in place.
#include "nova/material.glsl"

void material(inout Surface s) {
    vec3 tint = dot(s.custom.rgb, s.custom.rgb) > 0.0 ? s.custom.rgb : vec3(0.2, 0.9, 1.0);
    float density = s.custom.a > 0.0 ? s.custom.a : 18.0;

    float scan = 0.5 + 0.5 * sin((s.worldPos.y - nova_time() * 0.6) * density * 6.2831);
    scan = smoothstep(0.35, 1.0, scan);
    float rim = pow(1.0 - max(dot(s.normal, s.viewDir), 0.0), 3.0);

    s.albedo = mix(s.albedo, tint, 0.65);
    s.roughness = 0.25;
    s.emissive += tint * (rim * 3.0 + scan * 0.6);
}

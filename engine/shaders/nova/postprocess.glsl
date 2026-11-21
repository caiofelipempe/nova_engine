// Post-process shader interface. A post effect is a fullscreen fragment shader that runs on the
// HDR image (linear, before exposure and tonemapping), in the order effects were added:
//
//   #version 460
//   #include "nova/postprocess.glsl"
//
//   vec3 postProcess(vec2 uv) {
//       vec3 c = nova_scene(uv);
//       float d = length(uv - 0.5);
//       return c * mix(1.0, smoothstep(0.8, 0.2, d), nova_params().x);  // PostProcessDesc::params
//   }
#ifndef NOVA_POSTPROCESS_GLSL
#define NOVA_POSTPROCESS_GLSL
#include "nova/common.glsl"

// C++: nova::gpu::PostProcessPush.
layout(push_constant) uniform PostProcessPush {
    FrameBuffer frame;
    uint inputTexture; // bindless index of the HDR input
    uint _pad0;
    vec2 texelSize;    // 1 / resolution
    vec2 _pad1;
    vec4 params;       // PostProcessDesc::params (offset 32)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// The image produced by the previous pass (linear HDR).
vec3 nova_scene(vec2 uv) { return texture(g_textures[nonuniformEXT(pc.inputTexture)], uv).rgb; }
vec2 nova_texelSize() { return pc.texelSize; }
vec4 nova_params() { return pc.params; }
float nova_time() { return pc.frame.data.cameraPos.w; }
FrameData nova_frame() { return pc.frame.data; }

// Implemented by the effect.
vec3 postProcess(vec2 uv);

void main() { outColor = vec4(postProcess(vUV), 1.0); }

#endif // NOVA_POSTPROCESS_GLSL

// Cook-Torrance GGX, shared by the rasterized lighting (nova/lighting.glsl) and the ray tracing
// shaders. Public: game shaders may include it.
#ifndef NOVA_BRDF_GLSL
#define NOVA_BRDF_GLSL

const float NOVA_PI = 3.14159265359;

float nova_D_GGX(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (NOVA_PI * d * d);
}

float nova_G_SchlickGGX(float NdotX, float k) { return NdotX / (NdotX * (1.0 - k) + k); }

vec3 nova_F_Schlick(float VdotH, vec3 F0) { return F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0); }

// Cook-Torrance GGX; returns the radiance factor already multiplied by N.L.
vec3 nova_brdf(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float a = max(roughness * roughness, 0.002);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = nova_F_Schlick(VdotH, F0);
    float D = nova_D_GGX(NdotH, a);
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float G = nova_G_SchlickGGX(NdotV, k) * nova_G_SchlickGGX(NdotL, k);

    vec3 specular = D * G * F / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / NOVA_PI + specular) * NdotL;
}

// The hemisphere ambient light on a surface facing N (occlusion 1 = none).
vec3 nova_ambient(vec3 N, vec3 albedo, float metallic, vec3 sky, vec3 ground, float intensity, float occlusion) {
    vec3 ambient = mix(ground, sky, N.y * 0.5 + 0.5) * intensity;
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    return ambient * (albedo * (1.0 - metallic) + F0 * 0.25) * occlusion;
}

#endif // NOVA_BRDF_GLSL

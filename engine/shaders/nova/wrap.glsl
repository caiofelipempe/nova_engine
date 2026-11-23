// The laps of the circular world in shaders (RenderSettings::wrapShift). Positions are camera
// relative; a lap's center ("cell") is 0 for the lap around the camera.
#ifndef NOVA_WRAP_GLSL
#define NOVA_WRAP_GLSL

#define NOVA_WRAP_TRIANGLE 0u
#define NOVA_WRAP_OBJECT 1u
#define NOVA_WRAP_SPLIT 2u
#define NOVA_WRAP_CROP 3u
#define NOVA_WRAP_CROP_VIEW 4u // crop in the camera's own axes (NOVA_RENDER_ORIGIN_ROTATION builds)

// Whole laps that bring `d` (from the lap's center) within half a lap, on the circular axes.
vec3 nova_lapShift(vec3 d, vec3 size) {
    vec3 laps = floor(d / max(size, vec3(1e-6)) + 0.5);
    return mix(vec3(0.0), -laps * size, greaterThan(size, vec3(0.0)));
}

// The largest of |x|, |y|, |z| in the camera's axes: a distance whose "ball" is a box turning with
// the camera (`view`: world → view rotation).
float nova_turnedNorm(vec3 d, mat3 view) {
    vec3 v = abs(view * d);
    return max(v.x, max(v.y, v.z));
}

// Whole laps that bring `d` nearest the center measured in the camera's axes (the lap around the
// camera turning with it, RenderSettings::wrapTurnWithCamera). Every point has one such copy, so
// the turned laps still tile the world. Among the copy nearest on each axis and its neighbours: a
// farther one is never nearer. Same order and test as nova::turnedLapShift on the CPU.
vec3 nova_turnedLapShift(vec3 d, vec3 size, mat3 view) {
    vec3 base = nova_lapShift(d, size);
    vec3 best = base;
    float bestNorm = nova_turnedNorm(d + base, view);
    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j)
            for (int k = -1; k <= 1; ++k) {
                vec3 step = vec3(i, j, k);
                if ((i != 0 && size.x <= 0.0) || (j != 0 && size.y <= 0.0) || (k != 0 && size.z <= 0.0)) continue;
                vec3 shift = base + step * size;
                float n = nova_turnedNorm(d + shift, view);
                if (n < bestNorm) {
                    best = shift;
                    bestNorm = n;
                }
            }
    return best;
}

#endif

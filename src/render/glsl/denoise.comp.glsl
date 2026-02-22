#version 430 core

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0, rgba32f) readonly uniform image2D uInputColor;
layout(binding = 1, rgba32f) readonly uniform image2D uInputNormal;
layout(binding = 2, rgba32f) readonly uniform image2D uInputAlbedo;
layout(binding = 3, rgba32f) writeonly uniform image2D uOutputColor;

uniform ivec2 uImageSize;
uniform float uSigmaColor;
uniform float uSigmaNormal;
uniform float uSigmaAlbedo;
uniform int uRadius;

float gaussian(float x, float sigma) {
    return exp(-(x * x) / max(2.0 * sigma * sigma, 1e-5));
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x >= uImageSize.x || pixel.y >= uImageSize.y) {
        return;
    }

    vec3 centerColor = imageLoad(uInputColor, pixel).rgb;
    vec3 centerNormal = normalize(imageLoad(uInputNormal, pixel).rgb * 2.0 - 1.0);
    vec3 centerAlbedo = imageLoad(uInputAlbedo, pixel).rgb;

    vec3 accum = vec3(0.0);
    float weightSum = 0.0;
    for (int y = -uRadius; y <= uRadius; ++y) {
        for (int x = -uRadius; x <= uRadius; ++x) {
            ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), uImageSize - ivec2(1));
            vec3 c = imageLoad(uInputColor, q).rgb;
            vec3 n = normalize(imageLoad(uInputNormal, q).rgb * 2.0 - 1.0);
            vec3 a = imageLoad(uInputAlbedo, q).rgb;

            float spatialW = gaussian(length(vec2(x, y)), float(max(uRadius, 1)));
            float colorW = gaussian(length(c - centerColor), uSigmaColor);
            float normalW = gaussian(1.0 - max(dot(n, centerNormal), 0.0), uSigmaNormal);
            float albedoW = gaussian(length(a - centerAlbedo), uSigmaAlbedo);
            float w = spatialW * colorW * normalW * albedoW;

            accum += c * w;
            weightSum += w;
        }
    }

    vec3 denoised = weightSum > 1e-6 ? (accum / weightSum) : centerColor;
    imageStore(uOutputColor, pixel, vec4(denoised, 1.0));
}

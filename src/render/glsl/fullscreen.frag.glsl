#version 430 core

in vec2 vUv;
layout(location = 0) out vec4 fragColor;

uniform sampler2D uRawTexture;
uniform sampler2D uDenoisedTexture;
uniform int uUseDenoised;
uniform float uDenoiseBlend;

void main() {
    vec4 rawColor = texture(uRawTexture, vUv);
    if (uUseDenoised == 0) {
        fragColor = rawColor;
        return;
    }

    vec4 denoisedColor = texture(uDenoisedTexture, vUv);
    float blend = clamp(uDenoiseBlend, 0.0, 1.0);
    fragColor = mix(rawColor, denoisedColor, blend);
}

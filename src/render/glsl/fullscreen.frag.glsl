#version 430 core

in vec2 vUv;
layout(location = 0) out vec4 fragColor;

uniform sampler2D uOutputTexture;

void main() {
    fragColor = texture(uOutputTexture, vUv);
}

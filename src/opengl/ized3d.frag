#version 140
// SPDX-FileCopyrightText: 2002-2010 iZ3D Inc.
// SPDX-License-Identifier: MIT
// Analytic panel formulas from iZ3D, preserved in effcol's wiz3D.

in vec2 texcoord0;
out vec4 fragColor;

uniform sampler2D leftEye;
uniform sampler2D rightEye;
uniform int panel;
uniform int formula;
uniform int transfer;

vec3 decode(vec3 value)
{
    if (transfer == 1) {
        return value;
    }
    return mix(pow((value + 0.055) / 1.055, vec3(2.4)), value / 12.92, lessThanEqual(value, vec3(0.04045)));
}

vec3 encode(vec3 value)
{
    value = clamp(value, 0.0, 1.0);
    if (transfer == 1) {
        return value;
    }
    return mix(1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055, 12.92 * value, lessThanEqual(value, vec3(0.0031308)));
}

vec3 sampleEye(bool right, ivec2 pixel)
{
    ivec2 size = textureSize(leftEye, 0);
    pixel = (pixel + size) % size;
    return decode(right ? texelFetch(rightEye, pixel, 0).rgb : texelFetch(leftEye, pixel, 0).rgb);
}

void main()
{
    if (formula >= 4) {
        ivec2 pixel = ivec2(gl_FragCoord.xy);
        bool right = ((formula == 4 ? pixel.y : pixel.x + pixel.y) % 2) != 0;
        if (panel == 1) {
            fragColor = vec4(vec3(right ? 0.0 : 1.0), 1.0);
            return;
        }
        ivec2 source = ivec2(texcoord0 * vec2(textureSize(leftEye, 0)));
        vec3 value = sampleEye(right, source);
        if (formula == 4) {
            value = (value + sampleEye(right, source + ivec2(0, right ? -1 : 1))) * 0.5;
        }
        fragColor = vec4(encode(value), 1.0);
        return;
    }

    vec3 left = decode(texture(leftEye, texcoord0).rgb);
    vec3 right = decode(texture(rightEye, texcoord0).rgb);
    if (formula == 2) {
        const vec3 weights = vec3(0.2989, 0.5870, 0.1140);
        left = vec3(dot(left, weights));
        right = vec3(dot(right, weights));
    }
    vec3 sum = left + right;
    vec3 back = sum * 0.5;
    vec3 front = vec3(0.5);
    if (formula == 0) {
        float threshold = transfer == 1 ? 0.000001 : 0.0000000001;
        for (int channel = 0; channel < 3; ++channel) {
            if (sum[channel] >= threshold) {
                front[channel] = right[channel] / sum[channel];
            }
        }
    } else {
        float total = dot(sum, vec3(1.0));
        float modulation = total >= 0.01 ? dot(right, vec3(1.0)) / total : 0.5;
        front = vec3(modulation);
        if (formula == 3) {
            back = sum * (1.0 - modulation);
            front = sum * modulation;
        }
    }
    fragColor = vec4(encode(panel == 0 ? back : front), 1.0);
}

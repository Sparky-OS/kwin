#version 140
// SPDX-License-Identifier: GPL-2.0-or-later

// The depth picture: the desktop's own samples where they are known (alpha one); elsewhere
// (alpha zero) the sample of the estimated parallax where the estimate has matched the views, and
// the desktop's sample there otherwise: the level of the window the 3D content is in, whose plain
// areas its views can't place, or the screen's.

in vec2 texcoord0;
out vec4 fragColor;
uniform sampler2D desktop;
uniform sampler2D estimate;
uniform int hasEstimate;
uniform float scale;
uniform vec2 estimateSize;
uniform float zero;
uniform float samplesPerPixel;

// a texel of the estimate: the parallax plus 128, its whole part in red and its fraction in green,
// and in blue whether the views matched there
vec4 texelAt(ivec2 at)
{
    return texelFetch(estimate, clamp(at, ivec2(0), ivec2(estimateSize) - 1), 0);
}

float parallaxOf(vec4 coded)
{
    return coded.r * 255.0 + coded.g - 128.0;
}

void main()
{
    vec4 known = texelFetch(desktop, ivec2(gl_FragCoord.xy), 0);
    if (known.a > 0.5) {
        fragColor = vec4(known.rgb, 1.0);
        return;
    }
    if (hasEstimate == 0) {
        fragColor = vec4(known.rgb, 1.0);
        return;
    }
    // interpolated here, not by the texture unit: filtering an 8-bit texture rounds each channel,
    // which would break the sum of the whole part and the fraction. A texel stands for the square
    // around the pixel scale / 2 - 1 of its block; only the texels that matched count.
    vec2 at = (gl_FragCoord.xy - vec2(scale * 0.5 - 0.5)) / scale;
    ivec2 base = ivec2(floor(at));
    vec2 weight = at - floor(at);
    vec4 texels[4] = vec4[4](texelAt(base), texelAt(base + ivec2(1, 0)), texelAt(base + ivec2(0, 1)), texelAt(base + ivec2(1, 1)));
    float weights[4] = float[4]((1.0 - weight.x) * (1.0 - weight.y), weight.x * (1.0 - weight.y), (1.0 - weight.x) * weight.y, weight.x * weight.y);
    float matched = 0.0;
    float sum = 0.0;
    for (int i = 0; i < 4; ++i) {
        float w = weights[i] * step(0.5, texels[i].b);
        matched += w;
        sum += w * parallaxOf(texels[i]);
    }
    if (matched < 0.5) {
        fragColor = vec4(known.rgb, 1.0);
        return;
    }
    float level = clamp(floor(zero + sum / matched * samplesPerPixel + 0.5), 0.0, 255.0);
    fragColor = vec4(vec3(level / 255.0), 1.0);
}

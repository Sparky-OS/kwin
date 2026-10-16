#version 140
// SPDX-License-Identifier: GPL-2.0-or-later

// The parallax of each texel of the estimate, a scale x scale square of the picture: how far the
// other eye's picture is moved against this one, found by matching blocks along the eyes' axis.
// The result is the parallax in pixels plus 128, its whole part in red and its fraction in green,
// and in blue whether the match is one: a plain block, or a pattern that repeats along the axis,
// matches at more than one parallax, and its views don't tell its depth.

in vec2 texcoord0;
out vec4 fragColor;
uniform sampler2D picture;
uniform sampler2D other;
uniform ivec2 size;
// a pixel to the right on the output, in pixels of the pictures
uniform vec2 direction;
uniform int range;
uniform int scale;

float luma(vec4 color)
{
    return dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
}

// the sum of absolute differences over a block of 5 x 5 samples, two pixels apart
float cost(ivec2 centre, float parallax)
{
    vec2 shift = direction * parallax;
    float sum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            ivec2 at = clamp(centre + 2 * ivec2(x, y), ivec2(0), size - 1);
            ivec2 moved = clamp(ivec2(floor(vec2(at) + shift + 0.5)), ivec2(0), size - 1);
            sum += abs(luma(texelFetch(picture, at, 0)) - luma(texelFetch(other, moved, 0)));
        }
    }
    return sum;
}

void main()
{
    ivec2 centre = ivec2(gl_FragCoord.xy) * scale + scale / 2 - 1;
    // every pixel of the range: a coarser search misses the match of fine texture, whose cost is
    // low only at the right parallax
    float best = 0.0;
    float bestCost = cost(centre, 0.0);
    for (int parallax = -range; parallax <= range; ++parallax) {
        float c = cost(centre, float(parallax));
        if (c < bestCost) {
            bestCost = c;
            best = float(parallax);
        }
    }
    // the match is one when no parallax two pixels or more from the best costs about as little
    float matched = 1.0;
    for (int parallax = -range; parallax <= range; ++parallax) {
        if (abs(float(parallax) - best) >= 2.0 && cost(centre, float(parallax)) <= bestCost * 1.1 + 0.02) {
            matched = 0.0;
            break;
        }
    }
    // the fraction from the parabola through the costs around the best
    float before = cost(centre, best - 1.0);
    float after = cost(centre, best + 1.0);
    float curvature = before - 2.0 * bestCost + after;
    float fraction = curvature > 0.0 ? clamp(0.5 * (before - after) / curvature, -0.5, 0.5) : 0.0;
    float value = clamp(best + fraction + 128.0, 0.0, 255.99);
    fragColor = vec4(floor(value) / 255.0, fract(value), matched, 1.0);
}

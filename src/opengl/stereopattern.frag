#version 140
// SPDX-License-Identifier: GPL-2.0-or-later
#include "colormanagement.glsl"

in vec2 texcoord0;
out vec4 fragColor;
uniform sampler2D leftEye;
uniform sampler2D rightEye;
uniform int pattern;
uniform int rightFirst;

void main()
{
    // the scanout buffer is drawn with its first line at y 0
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int parity = pattern == 0 ? pixel.y : (pattern == 1 ? pixel.x : pixel.x + pixel.y);
    bool right = ((parity + rightFirst) % 2) != 0;
    vec4 color = right ? texture(rightEye, texcoord0) : texture(leftEye, texcoord0);
    fragColor = nitsToDestinationEncoding(sourceEncodingToNitsInDestinationColorspace(color));
}

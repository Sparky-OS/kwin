#version 140
// SPDX-License-Identifier: GPL-2.0-or-later
#include "colormanagement.glsl"

in vec2 texcoord0;
out vec4 fragColor;
uniform sampler2D leftEye;
uniform sampler2D rightEye;
uniform int pattern;
uniform int rightFirst;
// line alternative (pattern 3): the eyes' height, half the buffer's
uniform float eyeHeight;

void main()
{
    // the scanout buffer is drawn with its first line at y 0
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int parity = pattern == 0 || pattern == 3 ? pixel.y : (pattern == 1 ? pixel.x : pixel.x + pixel.y);
    bool right = ((parity + rightFirst) % 2) != 0;
    vec2 at = texcoord0;
    if (pattern == 3) {
        // each line of an eye, whole, on two lines of the buffer: the left eye's, then the right eye's
        at.y = (floor(texcoord0.y * eyeHeight) + 0.5) / eyeHeight;
    }
    vec4 color = right ? texture(rightEye, at) : texture(leftEye, at);
    fragColor = nitsToDestinationEncoding(sourceEncodingToNitsInDestinationColorspace(color));
}

#version 140
// SPDX-License-Identifier: GPL-2.0-or-later
// Red/cyan anaglyph: the two eyes mixed into one picture for any screen, with
// Eric Dubois's least-squares method (matrices set per display type by the caller).
// The mix is done in linear light, in the output's colour space.
#include "colormanagement.glsl"

in vec2 texcoord0;

out vec4 fragColor;

uniform sampler2D leftEye;
uniform sampler2D rightEye;

// out = leftMatrix * left + rightMatrix * right
uniform mat4 leftMatrix;
uniform mat4 rightMatrix;

void main()
{
    vec4 left = sourceEncodingToNitsInDestinationColorspace(texture(leftEye, texcoord0));
    vec4 right = sourceEncodingToNitsInDestinationColorspace(texture(rightEye, texcoord0));
    vec3 mixed = (leftMatrix * vec4(left.rgb, 0.0)).rgb + (rightMatrix * vec4(right.rgb, 0.0)).rgb;
    fragColor = nitsToDestinationEncoding(vec4(max(mixed, vec3(0.0)), 1.0));
}

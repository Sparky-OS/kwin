#version 140
// SPDX-License-Identifier: MIT
// iZ3D's ordinary analytic mode: the back input is the encoded average and
// the front input is the encoded modulation ratio.

in vec2 texcoord0;
out vec4 fragColor;

uniform sampler2D leftEye;
uniform sampler2D rightEye;
uniform int panel;

void main()
{
    vec3 left = texture(leftEye, texcoord0).rgb;
    vec3 right = texture(rightEye, texcoord0).rgb;
    vec3 sum = left + right;
    vec3 back = sum * 0.5;
    vec3 front = vec3(0.5);
    for (int channel = 0; channel < 3; ++channel) {
        if (sum[channel] >= 0.000001) {
            front[channel] = right[channel] / sum[channel];
        }
    }
    fragColor = vec4(panel == 0 ? back : front, 1.0);
}

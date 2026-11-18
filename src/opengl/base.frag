#version 140

#if GL_OES_standard_derivatives && ((TRAIT_ROUNDED_CORNERS && TRAIT_BORDER) || TRAIT_STEREO_AREA_FILTER)
#extension GL_OES_standard_derivatives : enable
#endif

#if TRAIT_MAP_TEXTURE || TRAIT_MAP_MULTI_PLANE_TEXTURE
uniform sampler2D sampler;
in vec2 texcoord0;
#endif

#if TRAIT_MAP_MULTI_PLANE_TEXTURE
uniform sampler2D sampler1;
#endif

#if TRAIT_MAP_EXTERNAL_TEXTURE
#extension GL_OES_EGL_image_external : require
uniform samplerExternalOES sampler;
in vec2 texcoord0;
#endif

#if TRAIT_UNIFORM_COLOR
uniform vec4 geometryColor;
#endif

#if TRAIT_BORDER || TRAIT_ROUNDED_CORNERS
#include "sdf.glsl"
uniform vec4 box;
uniform vec4 cornerRadius;
in vec2 position0;
#endif

#if TRAIT_BORDER
uniform vec4 geometryColor;
uniform int thickness;
#endif

#if TRAIT_YUV_CONVERSION
uniform mat4 yuvToRgb;
#endif

#if TRAIT_MODULATE
uniform vec4 modulation;
#endif

#if TRAIT_ADJUST_SATURATION
#include "saturation.glsl"
#endif

#if TRAIT_TRANSFORM_COLORSPACE
#include "colormanagement.glsl"
#endif

out vec4 fragColor;

#if TRAIT_STEREO_AREA_FILTER
uniform vec2 stereoTextureSize;
uniform vec4 stereoEyeBounds;
in vec2 stereoLocalCoord;

vec4 sampleStereo(vec2 position)
{
    vec2 origin = stereoEyeBounds.xy * stereoTextureSize;
    vec2 extent = (stereoEyeBounds.zw - stereoEyeBounds.xy) * stereoTextureSize;
    position = clamp(position, vec2(0.5), max(vec2(0.5), extent - vec2(0.5)));
#if TRAIT_MAP_TEXTURE && __VERSION__ >= 130
    // Interpolate in eye-local texels; atlas offsets must not change the weights.
    ivec2 offset = ivec2(floor(origin));
    vec2 texel = position - vec2(0.5) + fract(origin);
    ivec2 first = ivec2(floor(texel));
    ivec2 last = ivec2(ceil(origin + extent)) - ivec2(1) - offset;
    vec2 weight = fract(texel);
    vec4 a = texelFetch(sampler, offset + first, 0);
    vec4 b = texelFetch(sampler, offset + min(first + ivec2(1, 0), last), 0);
    vec4 c = texelFetch(sampler, offset + min(first + ivec2(0, 1), last), 0);
    vec4 d = texelFetch(sampler, offset + min(first + ivec2(1), last), 0);
    return mix(mix(a, b, weight.x), mix(c, d, weight.x), weight.y);
#else
    vec2 uv = (position + origin) / stereoTextureSize;
#if TRAIT_MAP_MULTI_PLANE_TEXTURE
    return vec4(texture(sampler, uv).x, texture(sampler1, uv).rg, 1.0);
#elif TRAIT_MAP_EXTERNAL_TEXTURE
    return texture2D(sampler, uv);
#else
    return texture(sampler, uv);
#endif
#endif
}

vec4 stereoAreaSample()
{
    vec2 footprint = max(fwidth(stereoLocalCoord), vec2(1.0));
    vec2 center = stereoLocalCoord;
    if (max(footprint.x, footprint.y) <= 1.0) {
        // Bilinear, without reaching into the other eye at its edge.
        return sampleStereo(center);
    }
    // Any shrink integrates the covered texel areas, clamping within this eye only;
    // an axis that is not shrunk covers one texel, which is bilinear.
    vec2 low = center - footprint * 0.5;
    vec2 high = center + footprint * 0.5;
    vec4 sum = vec4(0.0);
    // Subsampled chroma can change slope inside a luma texel pair.
#if TRAIT_MAP_MULTI_PLANE_TEXTURE
    const float step = 1.0;
#else
    const float step = 2.0;
#endif
    // One bilinear lookup integrates up to four texels with separable weights.
    for (float y = floor(low.y); y < ceil(high.y); y += step) {
        vec2 wy = max(vec2(0.0), min(vec2(y + 1.0, y + step), vec2(high.y))
                               - max(vec2(y, y + 1.0), vec2(low.y)));
        float weightY = wy.x + wy.y;
        for (float x = floor(low.x); x < ceil(high.x); x += step) {
            vec2 wx = max(vec2(0.0), min(vec2(x + 1.0, x + step), vec2(high.x))
                                   - max(vec2(x, x + 1.0), vec2(low.x)));
            float weightX = wx.x + wx.y;
            vec2 center = vec2(x, y) + vec2(0.5) + vec2(wx.y / weightX, wy.y / weightY);
            sum += sampleStereo(center) * (weightX * weightY);
        }
    }
    return sum / (footprint.x * footprint.y);
}
#endif

void main(void)
{
    vec4 result;

#if TRAIT_MAP_TEXTURE
    result = texture(sampler, texcoord0);
#endif
#if TRAIT_MAP_MULTI_PLANE_TEXTURE
    result = vec4(texture(sampler, texcoord0).x, texture(sampler1, texcoord0).rg, 1.0);
#endif
#if TRAIT_MAP_EXTERNAL_TEXTURE
    // external textures require texture2D for sampling
    result = texture2D(sampler, texcoord0);
#endif
#if TRAIT_STEREO_AREA_FILTER
    result = stereoAreaSample();
#endif
#if TRAIT_UNIFORM_COLOR
    result = geometryColor;
#endif

#if TRAIT_BORDER
    float inner = sdfRoundedBox(position0, box.xy, box.zw, cornerRadius);
    float outer = sdfRoundedBox(position0, box.xy, box.zw + vec2(thickness), cornerRadius + vec4(thickness));
    float f = sdfSubtract(outer, inner);
    float df = fwidth(f);
    result = geometryColor * (1.0 - clamp(0.5 + f / df, 0.0, 1.0));
#endif

#if TRAIT_YUV_CONVERSION
    result.rgb = (yuvToRgb * vec4(result.rgb, 1.0)).rgb;
#endif

#if TRAIT_ROUNDED_CORNERS
    float f = sdfRoundedBox(position0, box.xy, box.zw, cornerRadius);
    float df = fwidth(f);
    result *= 1.0 - clamp(0.5 + f / df, 0.0, 1.0);
#endif

#if TRAIT_TRANSFORM_COLORSPACE
    result = encodingToNits(result, sourceNamedTransferFunction, sourceTransferFunctionParams.x, sourceTransferFunctionParams.y);
    result.rgb = (colorimetryTransform * vec4(result.rgb, 1.0)).rgb;
#endif

#if TRAIT_ADJUST_SATURATION
    result = adjustSaturation(result);
#endif

#if TRAIT_MODULATE
    result *= modulation;
#endif

#if TRAIT_TRANSFORM_COLORSPACE
    result.rgb = doTonemapping(result.rgb);
    result = nitsToDestinationEncoding(result);
#endif

    fragColor = result;
}

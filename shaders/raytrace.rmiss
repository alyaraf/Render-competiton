#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : enable
#include "raycommon.glsl"

layout(location = 0) rayPayloadInEXT RayPayloadType payload;
layout(binding = 3, set = 1) uniform sampler2D textureSamplers[];

layout(push_constant) uniform Constants
{
  vec4  clearColorRt;
  vec3  lightPositionRt;
  float lightIntensityRt;
  int lightTypeRt;
  int frameID;
  int frameSize;
  float userParam0;
  float userParam1;
  float userParam2;
};

#define texture0 textureSamplers[0]
#define texture1 textureSamplers[1]
#define texture2 textureSamplers[2]
#define texture3 textureSamplers[3]
#define texture4 textureSamplers[4]
#define textureBackground textureSamplers[5]

void main()
{  /**** MISS SHADER ****/
  // CHANGED: background is now an HDR environment image instead of a flat
  // near-black color (was: payload.directLight = vec3(0.01, 0.01, 0.02);).
  // Sample data/background.hdr (bound as textureSamplers[5]/textureBackground)
  // using the escaped ray's world direction, so camera rays and bounce/reflection
  // rays that leave the scene pick up the sky. Shadow rays hit this same miss
  // shader, but raytraceTri.rchit/raytraceAabb.rchit always overwrite
  // payload.directLight with computed radiance right after the shadow trace
  // returns, so shadow occlusion logic is unaffected by this change.
  vec3 d = normalize(gl_WorldRayDirectionEXT);
  // Equirectangular mapping (Z-up world, matches camUp = vec3(0,0,1))
  float u = atan(d.y, d.x) / (2.0 * 3.14159265358979323846) + 0.5;
  // CHANGED: was acos(d.z)/PI, which rendered the sky upside down (the image's
  // top row, sky/up direction, ended up sampled at the bottom of the sphere and
  // vice versa). Flipped to put d.z = 1 (straight up) at v = 1 instead of v = 0.
  float v = 1.0 - acos(clamp(d.z, -1.0, 1.0)) / 3.14159265358979323846;
  payload.directLight = texture(textureBackground, vec2(u, v)).rgb;
  payload.shadowRayMiss = true;
  payload.nextRayOrigin = vec3(0.0); payload.nextRayDirection = vec3(0.0);

}

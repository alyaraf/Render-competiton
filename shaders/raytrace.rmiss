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

void main()
{  /**** MISS SHADER ****/
  payload.directLight = vec3(0.01, 0.01, 0.02); 
  payload.shadowRayMiss = true;
  payload.nextRayOrigin = vec3(0.0); payload.nextRayDirection = vec3(0.0);

}

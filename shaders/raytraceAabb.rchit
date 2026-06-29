#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_GOOGLE_include_directive : enable
#include "raycommon.glsl"
#include "wavefront.glsl"

hitAttributeEXT HitAttributeType hit;
const vec2 baryCoord = vec2(0.0, 0.0);

// clang-format off
layout(location = 0) rayPayloadInEXT RayPayloadType payload;

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevelAS;

layout(binding = 2, set = 1, scalar) buffer ScnDesc { sceneDesc i[]; } scnDesc;
layout(binding = 5, set = 1, scalar) buffer Vertices { Vertex v[]; } vertices[];
layout(binding = 6, set = 1) buffer Indices { uint i[]; } indices[];

layout(binding = 1, set = 1, scalar) buffer MatColorBufferObject { WaveFrontMaterial m[]; } materials[];
layout(binding = 3, set = 1) uniform sampler2D textureSamplers[];
layout(binding = 4, set = 1)  buffer MatIndexColorBuffer { int i[]; } matIndex[];
layout(binding = 7, set = 1, scalar) buffer allAabbs_ {Aabb i[];} allAabbs[];

#include "rayhelper.glsl"

// clang-format on
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
{  /**** CLOSEST-HIT SHADER ****/
  vec3 p0, p1, p2, n0, n1, n2;
  vec2 t0, t1, t2;
  gsnGetPositions(gl_InstanceID, gl_PrimitiveID, p0, p1, p2);
  gsnGetNormals(gl_InstanceID, gl_PrimitiveID, n0, n1, n2);
  gsnGetTexCoords(gl_InstanceID, gl_PrimitiveID, t0, t1, t2);

  vec3 barys = vec3(1.0f - baryCoord.x - baryCoord.y, baryCoord.x, baryCoord.y);
  vec3 localNormal = normalize(n0 * barys.x + n1 * barys.y + n2 * barys.z);
  vec3 localPosition = p0 * barys.x + p1 * barys.y + p2 * barys.z;
  vec2 texCoords = t0 * barys.x + t1 * barys.y + t2 * barys.z;

  mat3 normalMat;
  gsnGetNormal3x3Matrix(gl_InstanceID, normalMat);
  vec3 normal = normalize(normalMat * localNormal);
  vec3 position = gl_ObjectToWorldEXT * vec4(localPosition, 1.0);
  vec3 viewDir = normalize(-gl_WorldRayDirectionEXT);
  
  float metallicness = 0.0;
  float fresnelReflect = 0.5;
  float roughness = 0.1;
  float clearcoat = 0.0; // New parameter
  vec3 emission = vec3(0.0);
  vec3 baseColor = vec3(0.8);
  bool isRefractive = false;

  if(gl_InstanceID == 2) { // VeachPlanes - artistic cosine-gradient palette
    vec3 paletteA = vec3(0.55, 0.45, 0.65);
    vec3 paletteB = vec3(0.45, 0.45, 0.35);
    vec3 paletteC = vec3(1.0, 1.0, 0.6);
    vec3 paletteD = vec3(0.10, 0.45, 0.75);
    float t = position.x * 0.6 + position.y * 0.35 + position.z * 0.2;
    baseColor = clamp(paletteA + paletteB * cos(2.0 * PI * (paletteC * t + paletteD)), 0.0, 1.0);
    roughness = 0.2;
    fresnelReflect = 0.5;
    clearcoat = 0.4;
  }
  else if(gl_InstanceID == 0) { // DiscoBot (blas0) - shimmering disco-ball cosine palette
    vec3 discoA = vec3(0.5, 0.5, 0.5);
    vec3 discoB = vec3(0.5, 0.5, 0.5);
    vec3 discoC = vec3(2.0, 1.5, 1.0);
    vec3 discoD = vec3(0.0, 0.25, 0.5);
    float dt = normal.x * 0.5 + normal.y * 0.5 + normal.z * 0.3 + position.z * 0.4;
    baseColor = clamp(discoA + discoB * cos(2.0 * PI * (discoC * dt + discoD)), 0.0, 1.0);
    metallicness = 0.6;
    roughness = 0.15;
    fresnelReflect = 0.7;
    clearcoat = 0.6; // shiny disco-ball sheen
  }
  else if(gl_InstanceID == 1) { // StoneDemon (blas1) - molten/hellish cosine palette
    vec3 demonA = vec3(0.45, 0.20, 0.15);
    vec3 demonB = vec3(0.45, 0.25, 0.10);
    vec3 demonC = vec3(1.2, 1.0, 0.8);
    vec3 demonD = vec3(0.0, 0.10, 0.30);
    float mt = position.z * 0.8 + normal.y * 0.3;
    baseColor = clamp(demonA + demonB * cos(2.0 * PI * (demonC * mt + demonD)), 0.0, 1.0);
    metallicness = 0.1;
    roughness = 0.35;
    fresnelReflect = 0.5;
    clearcoat = 0.3; // faint molten sheen
  }

  vec3 lightPos = vec3(0.5, -2.0, 0.7) + normalize(random_pcg3d(uvec3(frameID, gl_LaunchIDEXT.xy))) * 0.2;
  vec3 lightDir = normalize(lightPos - position);
  float frontFacing = dot(-gl_WorldRayDirectionEXT, normal);
  
  payload.shadowRayMiss = false;
  traceRayEXT(topLevelAS, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT, 0xFFu, 0u, 0u, 0u, 
      position + sign(frontFacing) * 0.001 * normal, 0.001, lightDir, length(lightPos - position), 0);
  
  vec3 radiance = vec3(0.0); 
  if(payload.shadowRayMiss || frontFacing < 0.0) { 
    float irradiance = max(dot(lightDir, normal), 0.0) * (4.0 * PI * 0.04) * (1.0 / pow(max(length(lightPos - position), 0.1), 2.0)) * 200.0;
    if(irradiance > 0.0) radiance += microfacetBRDF(lightDir, viewDir, normal, baseColor, metallicness, fresnelReflect, roughness, clearcoat) * irradiance; 
  }  
  
  payload.directLight = (payload.level > 0) ? clamp(radiance, 0.0, 5.0) + emission : radiance + emission;
  
  if (isRefractive) {
      vec3 outwardNormal = (frontFacing < 0.0) ? -normal : normal;
      vec3 refrDir = refract(-viewDir, outwardNormal, (frontFacing < 0.0) ? 1.52 : 1.0 / 1.52);
      payload.nextRayOrigin = position - outwardNormal * 0.002;
      payload.nextRayDirection = (refrDir == vec3(0.0)) ? reflect(-viewDir, normal) : refrDir;
      payload.nextFactor = vec3(0.98); 
  } else {
      payload.nextRayOrigin = position;
      payload.nextRayDirection = sampleMicrofacetBRDF(viewDir, normal, baseColor, metallicness, fresnelReflect, roughness, random_pcg3d(uvec3(gl_LaunchIDEXT.xy, frameID + payload.level)), payload.nextFactor);
  }

}

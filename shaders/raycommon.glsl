struct Aabb
{
  vec3 minimum;
  vec3 maximum;
};

 // gsnShaderOptions: precompiler="GL_EXT_ray_tracing, recursion: 2, app: 1"
// IMPORTANT: do not remove the line above

/**** ABOUT ****/
// Advanced Path Tracing with Importance Sampling and Dual-Lobe PBR

/**** COMMON START ****/
struct RayPayloadType {
  vec3 directLight;
  vec3 nextRayOrigin;
  vec3 nextRayDirection;
  vec3 nextFactor;
  bool shadowRayMiss;
  int level;
}; 

struct HitAttributeType {
  vec3 normal; 
}; 

const vec3 camPos = vec3(0.0f, -4.75f, 0.0f);
const vec3 camLookAt = vec3(0.0f, 0.0f, 0.0f);
const vec3 camUp = vec3(0.0, 0.0, 1.0);
const float PI = 3.14159265359;

vec3 getCameraRayLookAt(float fieldOfViewY, float aspectRatio, 
                        vec3 eye, vec3 ref, vec3 up, vec2 point) {
  float focalLength = 1.0 / tan(0.5 * fieldOfViewY * PI / 180.0);
  vec2 pos = 2.0 * (point - 0.5);
  vec3 rayCam = vec3(pos.x * aspectRatio, pos.y, -focalLength);
  
  vec3 camZ = normalize(eye - ref);
  vec3 v = normalize(up);
  vec3 camX = cross(v, camZ);
  vec3 camY = cross(camZ, camX);
  
  return normalize(camX * rayCam.x + camY * rayCam.y + camZ * rayCam.z);
}

float radicalInverse(uint bits) {
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return float(bits) * 2.3283064365386963e-10; 
}

vec2 hammersley(uint n, uint N) {
  return vec2((float(n) + 0.5) / float(N), radicalInverse(n + 1u));
}

vec3 random_pcg3d(uvec3 v) {
  v = v * 1664525u + 1013904223u;
  v.x += v.y*v.z; v.y += v.z*v.x; v.z += v.x*v.y;
  v ^= v >> 16u;
  v.x += v.y*v.z; v.y += v.z*v.x; v.z += v.x*v.y;
  return vec3(v) * (1.0/float(0xffffffffu));
}

mat3 getNormalSpace(in vec3 normal) {
   vec3 someVec = vec3(1.0, 0.0, 0.0);
   float dd = dot(someVec, normal);
   vec3 tangent = vec3(0.0, 1.0, 0.0);
   if(1.0 - abs(dd) > 1e-6) tangent = normalize(cross(someVec, normal));
   vec3 bitangent = cross(normal, tangent);
   return mat3(tangent, bitangent, normal);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
  return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
} 

float D_GGX(float NoH, float roughness) {
  float alpha = roughness * roughness;
  float alpha2 = alpha * alpha;
  float NoH2 = NoH * NoH;
  float b = (NoH2 * (alpha2 - 1.0) + 1.0);
  return alpha2 / (PI * b * b);
}

float G1_GGX_Schlick(float NdotV, float roughness) {
  float r = 0.5 + 0.5 * roughness; 
  float k = (r * r) / 2.0;
  float denom = NdotV * (1.0 - k) + k;
  return NdotV / denom;
}

float G_Smith(float NoV, float NoL, float roughness) {
  return G1_GGX_Schlick(NoL, roughness) * G1_GGX_Schlick(NoV, roughness);
}

// UPGRADE: Advanced BRDF with a Clear-Coat layer calculation
vec3 microfacetBRDF(in vec3 L, in vec3 V, in vec3 N, 
              in vec3 baseColor, in float metallicness, 
              in float fresnelReflect, in float roughness, in float clearcoat) {
      
  vec3 H = normalize(V + L); 
  float NoV = clamp(dot(N, V), 0.0, 1.0);
  float NoL = clamp(dot(N, L), 0.0, 1.0);
  float NoH = clamp(dot(N, H), 0.0, 1.0);
  float VoH = clamp(dot(V, H), 0.0, 1.0);     
  
  // Base Specular Lobe
  vec3 f0 = mix(vec3(0.16 * (fresnelReflect * fresnelReflect)), baseColor, metallicness);
  vec3 F = fresnelSchlick(VoH, f0);
  float D = D_GGX(NoH, roughness);
  float G = G_Smith(NoV, NoL, roughness);
  vec3 baseSpec = (D * G * F) / max(4.0 * NoV * NoL, 0.001);
  
  // UPGRADE: Secondary Clear-Coat Lobe (fixed IOR 1.5 -> F0 = 0.04)
  float ccRoughness = 0.05; // very smooth top layer
  vec3 ccF = fresnelSchlick(VoH, vec3(0.04)) * clearcoat;
  float ccD = D_GGX(NoH, ccRoughness);
  float ccG = G_Smith(NoV, NoL, ccRoughness);
  vec3 clearcoatSpec = (ccD * ccG * ccF) / max(4.0 * NoV * NoL, 0.001);

  // Energy Conservation & Diffuse
  vec3 notSpec = vec3(1.0) - F; 
  notSpec *= 1.0 - metallicness; 
  // Attenuate base layer by the clearcoat transmission
  vec3 diff = notSpec * baseColor / PI * (vec3(1.0) - ccF); 
  
  return diff + baseSpec + clearcoatSpec;
}

// UPGRADE: Cosine-Weighted Importance Sampling
vec3 sampleMicrofacetBRDF(in vec3 V, in vec3 N, 
              in vec3 baseColor, in float metallicness, 
              in float fresnelReflect, in float roughness, in vec3 random, out vec3 nextFactor) 
{
  if(random.z > 0.5) {
    // Diffuse: Cosine-weighted hemisphere sampling (solves PDF = cosTheta / PI)
    float r1 = random.x;
    float r2 = random.y;
    float phi = 2.0 * PI * r1;
    float cosTheta = sqrt(1.0 - r2);
    float sinTheta = sqrt(r2);
    
    vec3 localDiffuseDir = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    vec3 L = getNormalSpace(N) * localDiffuseDir;  
    
    vec3 H = normalize(V + L);
    float VoH = clamp(dot(V, H), 0.0, 1.0);     
    
    vec3 f0 = mix(vec3(0.16 * (fresnelReflect * fresnelReflect)), baseColor, metallicness);    
    vec3 F = fresnelSchlick(VoH, f0);
    vec3 notSpec = (vec3(1.0) - F) * (1.0 - metallicness); 
  
    // PDF cancels out with the PI denominator, leaving just the albedo
    nextFactor = notSpec * baseColor * 2.0; 
    return L;
    
  } else {
    // Specular: GGX Importance Sampling
    float a = roughness * roughness;
    float phi = 2.0 * PI * random.x;
    float cosTheta = sqrt((1.0 - random.y) / (1.0 + (a * a - 1.0) * random.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    
    vec3 localH = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    vec3 H = getNormalSpace(N) * localH;  
    vec3 L = reflect(-V, H);

    float NoV = clamp(dot(N, V), 0.0, 1.0);
    float NoL = clamp(dot(N, L), 0.0, 1.0);
    float NoH = clamp(dot(N, H), 0.0, 1.0);
    float VoH = clamp(dot(V, H), 0.0, 1.0);     
    
    vec3 f0 = mix(vec3(0.16 * (fresnelReflect * fresnelReflect)), baseColor, metallicness);
    vec3 F = fresnelSchlick(VoH, f0);
    float G = G_Smith(NoV, NoL, roughness);
    
    nextFactor = (F * G * VoH / max(NoH * NoV, 0.001)) * 2.0;
    return L;
  }
}

// UPGRADE: Depth of Field (camera lens) settings
// CHANGED: aperture was 0.045, which blurred the background HDR so heavily it
// barely showed any detail. Lowered to keep DOF subtle while still showing the
// background environment clearly.
const float aperture  = 0.012; // lens radius: bigger = stronger blur
const float focusDist = 4.2;   // distance from camera to the in-focus plane

// UPGRADE: Procedural marble (turbulent veins) and wood (concentric rings) materials
float hash13(vec3 p) {
  p = fract(p * 0.3183099 + 0.1);
  p *= 17.0;
  return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float valueNoise3(vec3 p) {
  vec3 i = floor(p);
  vec3 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(hash13(i + vec3(0, 0, 0)), hash13(i + vec3(1, 0, 0)), f.x),
                  mix(hash13(i + vec3(0, 1, 0)), hash13(i + vec3(1, 1, 0)), f.x), f.y),
              mix(mix(hash13(i + vec3(0, 0, 1)), hash13(i + vec3(1, 0, 1)), f.x),
                  mix(hash13(i + vec3(0, 1, 1)), hash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

float fbm3(vec3 p) {
  float v = 0.0; float amp = 0.5;
  for(int i = 0; i < 5; i++) {
    v += amp * valueNoise3(p);
    p *= 2.0; amp *= 0.5;
  }
  return v;
}

vec3 marbleColor(vec3 p, vec3 baseTint, vec3 veinTint) {
  float n = fbm3(p * 3.0);
  float vein = sin((p.x + p.y * 0.3) * 8.0 + n * 12.0) * 0.5 + 0.5;
  vein = pow(vein, 3.0);
  return mix(baseTint, veinTint, vein);
}

vec3 woodColor(vec3 p, vec3 lightWood, vec3 darkWood) {
  float n = fbm3(p * 1.5) * 0.6;
  float radius = length(p.xy) + n;
  float rings = fract(radius * 6.0);
  rings = smoothstep(0.0, 0.15, rings) * smoothstep(1.0, 0.85, rings);
  return mix(darkWood, lightWood, rings);
}

// NEW: tangent-space normal mapping, used to apply the concrete wall normal map
// (data/concreteNorm.png) to DiscoBot and StoneDemon in raytraceTri.rchit /
// raytraceAabb.rchit. Builds a per-triangle tangent frame from the triangle's
// world-space edges and UV deltas (no precomputed vertex tangents needed).
mat3 cotangentFrame(in vec3 N, in vec3 dp1, in vec3 dp2, in vec2 duv1, in vec2 duv2)
{
    vec3 dp2perp = cross( dp2, N );
    vec3 dp1perp = cross( N, dp1 );
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt( max( dot(T,T), dot(B,B) ) );
    return mat3( T * invmax, B * invmax, N );
}

vec3 applyNormalMap(in sampler2D normalmap, vec2 texcoord, in vec3 normal, in vec3 edge0, in vec3 edge1, in vec2 texDiff0, in vec2 texDiff1)
{
    vec3 highResNormal = textureLod(normalmap, texcoord, 0.0).rgb;
    highResNormal = normalize(highResNormal * 2.0 - 1.0);
    mat3 TBN = cotangentFrame(normal, edge0, edge1, texDiff0, texDiff1);
    return normalize(TBN * highResNormal);
}
/**** COMMON END ****/


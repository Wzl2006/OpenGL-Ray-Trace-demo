#version 430 core

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

struct GpuBvhNode {
    vec4 bboxMin;
    vec4 bboxMax;
    ivec4 meta; // left, right, firstTriangle, triangleCount
};

struct GpuTriangle {
    vec4 v0;
    vec4 v1;
    vec4 v2;
    vec4 normalMaterial; // xyz normal, w material index
};

struct GpuSphere {
    vec4 centerRadius; // xyz center, w radius
    ivec4 meta;        // x material index
};

struct GpuMaterial {
    vec4 albedoRoughness;      // xyz albedo, w roughness
    vec4 emissionTransmission; // xyz emission, w transmission
    vec4 iorAbsorption;        // x ior, yzw absorption
    vec4 options;              // x diffuseEnabled(0/1)
};

layout(std430, binding = 0) readonly buffer BvhNodes {
    GpuBvhNode nodes[];
} uBvh;

layout(std430, binding = 1) readonly buffer Triangles {
    GpuTriangle triangles[];
} uTriangles;

layout(std430, binding = 2) readonly buffer Spheres {
    GpuSphere spheres[];
} uSpheres;

layout(std430, binding = 3) readonly buffer Materials {
    GpuMaterial materials[];
} uMaterials;

layout(std140, binding = 0) uniform CameraBlock {
    mat4 invView;
    mat4 invProj;
    vec4 cameraPosition;
    vec4 viewportAndFrame; // x width, y height, z frameIndex
} uCamera;

layout(std140, binding = 1) uniform ParamsBlock {
    ivec4 counts;       // triCount, nodeCount, sphereCount, materialCount
    ivec4 options;      // maxBounces, lightSamples, sppPerPixel, forceReset
    vec4 alphaAndMode;  // x alpha
    vec4 cauchy;        // x A, y B, z bandCount
    vec4 debug;         // x monochromaticMode(0/1), y wavelengthNm
} uParams;

layout(std140, binding = 2) uniform LightBlock {
    vec4 centerIntensity; // xyz center, w intensity
    vec4 colorSizeX;      // xyz color, w sizeX
    vec4 normalSizeY;     // xyz normal, w sizeY
} uLight;

layout(binding = 0, rgba32f) uniform image2D uAccumImage;
layout(binding = 1, rgba32f) uniform image2D uOutputImage;
layout(binding = 2, rgba32f) uniform image2D uBeautyImage;
layout(binding = 3, rgba32f) uniform image2D uNormalImage;
layout(binding = 4, rgba32f) uniform image2D uAlbedoImage;

struct PathLdsSampler {
    uint pixelSeed;
    uint sampleIndex;
    uint frameIndex;
};

const uint kDimsPerBounce = 12u;
const uint kDimBranch = 0u;
const uint kDimWavelength = 1u;
const uint kDimGgxReflect = 2u;
const uint kDimGgxRefract = 4u;
const uint kDimDiffuse = 6u;
const uint kDimLowProbRr = 8u;
const uint kDimThroughputRr = 9u;
const uint kDimLightScramble = 10u;

uint bounceDimBase(int bounce) {
    return uint(max(bounce, 0)) * kDimsPerBounce;
}

uint hash32(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAau) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley2D(uint i, uint n, uint scramble) {
    float u = (float(i) + 0.5) / max(float(n), 1.0);
    float v = radicalInverseVdC(i ^ scramble);
    return vec2(u, v);
}

uint ldsScramble(PathLdsSampler sampler, uint dim) {
    uint h = sampler.pixelSeed;
    h ^= hash32((sampler.frameIndex + 1u) * 0x9e3779b9u);
    h ^= hash32((dim + 1u) * 0x85ebca6bu);
    return hash32(h);
}

float lds1D(PathLdsSampler sampler, uint dim) {
    uint index = sampler.sampleIndex;
    uint scramble = ldsScramble(sampler, dim);
    return radicalInverseVdC(index ^ scramble);
}

vec2 lds2D(PathLdsSampler sampler, uint dim0) {
    return vec2(lds1D(sampler, dim0), lds1D(sampler, dim0 + 1u));
}

float saturate(float x) {
    return clamp(x, 0.0, 1.0);
}

const float kPi = 3.14159265;

float luminance(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 clampLuminance(vec3 c, float maxLum) {
    float lum = luminance(c);
    if (lum > maxLum && maxLum > 0.0) {
        c *= maxLum / lum;
    }
    return c;
}

float max3(vec3 v) {
    return max(v.x, max(v.y, v.z));
}

float powerHeuristic(float aPdf, float bPdf) {
    float a2 = aPdf * aPdf;
    float b2 = bPdf * bPdf;
    return a2 / max(a2 + b2, 1e-6);
}

float fresnelSchlick(float cosTheta, float F0);

float ggxD(float nDotH, float roughness) {
    float a = max(roughness * roughness, 0.001);
    float a2 = a * a;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1e-6);
}

float smithG1Schlick(float nDotX, float roughness) {
    float a = max(roughness, 0.001);
    float k = ((a + 1.0) * (a + 1.0)) * 0.125;
    return nDotX / max(nDotX * (1.0 - k) + k, 1e-6);
}

float ggxSpecularBrdf(vec3 n, vec3 v, vec3 l, float roughness, float f0) {
    float nDotV = max(dot(n, v), 0.0);
    float nDotL = max(dot(n, l), 0.0);
    if (nDotV <= 0.0 || nDotL <= 0.0) {
        return 0.0;
    }
    vec3 h = normalize(v + l);
    float nDotH = max(dot(n, h), 0.0);
    float vDotH = max(dot(v, h), 0.0);
    float D = ggxD(nDotH, roughness);
    float G = smithG1Schlick(nDotV, roughness) * smithG1Schlick(nDotL, roughness);
    float F = fresnelSchlick(vDotH, f0);
    return (D * G * F) / max(4.0 * nDotV * nDotL, 1e-6);
}

float ggxSpecularPdf(vec3 n, vec3 v, vec3 l, float roughness) {
    float nDotV = max(dot(n, v), 0.0);
    float nDotL = max(dot(n, l), 0.0);
    if (nDotV <= 0.0 || nDotL <= 0.0) {
        return 0.0;
    }
    vec3 h = normalize(v + l);
    float nDotH = max(dot(n, h), 0.0);
    float vDotH = max(dot(v, h), 0.0);
    if (vDotH <= 1e-6) {
        return 0.0;
    }
    float D = ggxD(nDotH, roughness);
    return (D * nDotH) / max(4.0 * vDotH, 1e-6);
}

struct Ray {
    vec3 origin;
    vec3 dir;
};

struct Hit {
    float t;
    vec3 position;
    vec3 geometricNormal;
    vec3 shadingNormal;
    int materialIndex;
    int frontFace;
};

struct PathResult {
    vec3 radiance;
    vec3 primaryNormal;
    vec3 primaryAlbedo;
    int hasPrimary;
};

float fresnelSchlick(float cosTheta, float F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 basisTangent(vec3 n) {
    vec3 up = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    return normalize(cross(up, n));
}

vec3 sampleHemisphere(vec3 n, vec2 xi) {
    float u1 = xi.x;
    float u2 = xi.y;
    float r = sqrt(u1);
    float phi = 6.28318530718 * u2;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - u1));
    vec3 t = basisTangent(n);
    vec3 b = cross(n, t);
    return normalize(t * x + b * y + n * z);
}

vec3 sampleGGXNormal(vec3 n, float roughness, vec2 xi) {
    float a = max(roughness * roughness, 0.001);
    float u1 = xi.x;
    float u2 = xi.y;
    float phi = 6.28318530718 * u1;
    float cosTheta = sqrt((1.0 - u2) / (1.0 + (a * a - 1.0) * u2));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));

    vec3 t = basisTangent(n);
    vec3 b = cross(n, t);
    return normalize(t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + n * cosTheta);
}

vec3 offsetRayOrigin(vec3 p, vec3 n, vec3 dir) {
    const float eps = 0.001;
    float signDir = dot(dir, n) >= 0.0 ? 1.0 : -1.0;
    return p + n * eps * signDir;
}

bool intersectAabb(Ray ray, vec3 bmin, vec3 bmax, float tMin, float tMax) {
    vec3 invDir = 1.0 / ray.dir;
    vec3 t0s = (bmin - ray.origin) * invDir;
    vec3 t1s = (bmax - ray.origin) * invDir;
    vec3 tsmaller = min(t0s, t1s);
    vec3 tbigger = max(t0s, t1s);
    float tEnter = max(max(tsmaller.x, tsmaller.y), max(tsmaller.z, tMin));
    float tExit = min(min(tbigger.x, tbigger.y), min(tbigger.z, tMax));
    return tExit >= tEnter;
}

bool intersectTriangle(Ray ray, GpuTriangle tri, float tMin, float tMax, out float tHit, out vec3 geomNormal, out int material) {
    vec3 v0 = tri.v0.xyz;
    vec3 v1 = tri.v1.xyz;
    vec3 v2 = tri.v2.xyz;
    vec3 edge1 = v1 - v0;
    vec3 edge2 = v2 - v0;
    vec3 n = normalize(cross(edge1, edge2));

    // Cornell walls are one-sided and only visible from the inside.
    if (dot(ray.dir, n) >= 0.0) {
        return false;
    }

    vec3 pvec = cross(ray.dir, edge2);
    float det = dot(edge1, pvec);
    if (abs(det) < 1e-7) {
        return false;
    }
    float invDet = 1.0 / det;
    vec3 tvec = ray.origin - v0;
    float u = dot(tvec, pvec) * invDet;
    if (u < 0.0 || u > 1.0) {
        return false;
    }
    vec3 qvec = cross(tvec, edge1);
    float v = dot(ray.dir, qvec) * invDet;
    if (v < 0.0 || u + v > 1.0) {
        return false;
    }
    float t = dot(edge2, qvec) * invDet;
    if (t <= tMin || t >= tMax) {
        return false;
    }

    tHit = t;
    geomNormal = n;
    material = int(round(tri.normalMaterial.w));
    return true;
}

bool intersectSphere(Ray ray, GpuSphere sphere, float tMin, float tMax, out float tHit, out vec3 geomNormal, out int material, out int frontFace) {
    vec3 center = sphere.centerRadius.xyz;
    float radius = sphere.centerRadius.w;
    vec3 oc = ray.origin - center;
    float a = dot(ray.dir, ray.dir);
    float b = dot(oc, ray.dir);
    float c = dot(oc, oc) - radius * radius;
    float discriminant = b * b - a * c;
    if (discriminant < 0.0) {
        return false;
    }

    float sqrtDisc = sqrt(discriminant);
    float root = (-b - sqrtDisc) / a;
    if (root <= tMin || root >= tMax) {
        root = (-b + sqrtDisc) / a;
        if (root <= tMin || root >= tMax) {
            return false;
        }
    }

    tHit = root;
    vec3 p = ray.origin + tHit * ray.dir;
    vec3 outwardNormal = normalize(p - center);
    frontFace = dot(ray.dir, outwardNormal) < 0.0 ? 1 : 0;
    geomNormal = outwardNormal;
    material = sphere.meta.x;
    return true;
}

bool intersectScene(Ray ray, float tMin, float tMax, out Hit hit) {
    bool found = false;
    float closest = tMax;
    vec3 geomNormal = vec3(0.0);
    int material = -1;
    int frontFace = 1;

    int stack[64];
    int stackPtr = 0;
    if (uParams.counts.y > 0) {
        stack[stackPtr++] = 0;
    }

    while (stackPtr > 0) {
        int nodeIndex = stack[--stackPtr];
        if (nodeIndex < 0 || nodeIndex >= uParams.counts.y) {
            continue;
        }

        GpuBvhNode node = uBvh.nodes[nodeIndex];
        if (!intersectAabb(ray, node.bboxMin.xyz, node.bboxMax.xyz, tMin, closest)) {
            continue;
        }

        if (node.meta.w > 0) {
            int first = node.meta.z;
            int count = node.meta.w;
            for (int i = 0; i < count; ++i) {
                float tTri;
                vec3 nTri;
                int mTri;
                if (intersectTriangle(ray, uTriangles.triangles[first + i], tMin, closest, tTri, nTri, mTri)) {
                    closest = tTri;
                    geomNormal = nTri;
                    material = mTri;
                    frontFace = 1;
                    found = true;
                }
            }
        } else {
            if (node.meta.x >= 0 && stackPtr < 63) {
                stack[stackPtr++] = node.meta.x;
            }
            if (node.meta.y >= 0 && stackPtr < 63) {
                stack[stackPtr++] = node.meta.y;
            }
        }
    }

    for (int i = 0; i < uParams.counts.z; ++i) {
        float tSphere;
        vec3 nSphere;
        int mSphere;
        int ff;
        if (intersectSphere(ray, uSpheres.spheres[i], tMin, closest, tSphere, nSphere, mSphere, ff)) {
            closest = tSphere;
            geomNormal = nSphere;
            material = mSphere;
            frontFace = ff;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    hit.t = closest;
    hit.position = ray.origin + ray.dir * closest;
    hit.geometricNormal = normalize(geomNormal);
    hit.frontFace = frontFace;
    hit.shadingNormal = frontFace == 1 ? hit.geometricNormal : -hit.geometricNormal;
    hit.materialIndex = material;
    return true;
}

float wavelengthNm(int index) {
    const float bands[6] = float[](430.0, 470.0, 510.0, 550.0, 600.0, 650.0);
    return bands[clamp(index, 0, 5)];
}

vec3 wavelengthToRgb(float lambda) {
    vec3 c = vec3(0.0);
    if (lambda >= 380.0 && lambda < 440.0) {
        c.r = -(lambda - 440.0) / (440.0 - 380.0);
        c.b = 1.0;
    } else if (lambda < 490.0) {
        c.g = (lambda - 440.0) / (490.0 - 440.0);
        c.b = 1.0;
    } else if (lambda < 510.0) {
        c.g = 1.0;
        c.b = -(lambda - 510.0) / (510.0 - 490.0);
    } else if (lambda < 580.0) {
        c.r = (lambda - 510.0) / (580.0 - 510.0);
        c.g = 1.0;
    } else if (lambda < 645.0) {
        c.r = 1.0;
        c.g = -(lambda - 645.0) / (645.0 - 580.0);
    } else if (lambda <= 780.0) {
        c.r = 1.0;
    }

    float attenuation = 1.0;
    if (lambda > 700.0) {
        attenuation = 0.3 + 0.7 * (780.0 - lambda) / (780.0 - 700.0);
    } else if (lambda < 420.0) {
        attenuation = 0.3 + 0.7 * (lambda - 380.0) / (420.0 - 380.0);
    }
    return c * attenuation;
}

bool occluded(vec3 origin, vec3 dir, float maxDistance) {
    Hit shadowHit;
    Ray shadowRay;
    shadowRay.origin = origin;
    shadowRay.dir = dir;
    return intersectScene(shadowRay, 0.001, maxDistance - 0.001, shadowHit);
}

vec3 sampleDirectLightNEE(in Hit hit,
                          in GpuMaterial material,
                          vec3 viewDir,
                          float diffuseWeight,
                          float specularWeight,
                          float roughness,
                          float f0,
                          PathLdsSampler sampler,
                          int bounce) {
    if (diffuseWeight <= 1e-5 && specularWeight <= 1e-5) {
        return vec3(0.0);
    }

    int sampleCount = clamp(uParams.options.y, 1, 32);
    vec3 lightCenter = uLight.centerIntensity.xyz;
    vec3 lightNormal = normalize(uLight.normalSizeY.xyz);
    float sizeX = uLight.colorSizeX.w;
    float sizeY = uLight.normalSizeY.w;
    float area = max(sizeX * sizeY, 1e-4);
    vec3 lightRadiance = uLight.colorSizeX.xyz * uLight.centerIntensity.w;

    vec3 result = vec3(0.0);
    vec3 diffuseBrdf = material.albedoRoughness.rgb * diffuseWeight * (1.0 / kPi);
    uint dimBase = bounceDimBase(bounce);
    uint scramble = hash32(ldsScramble(sampler, dimBase + kDimLightScramble));

    for (int i = 0; i < sampleCount; ++i) {
        // Per-pixel low-discrepancy light samples to avoid shared-workgroup correlations.
        vec2 uv = hammersley2D(uint(i), uint(sampleCount), scramble);
        vec3 lightPos = lightCenter + vec3((uv.x - 0.5) * sizeX, 0.0, (uv.y - 0.5) * sizeY);
        vec3 toLight = lightPos - hit.position;
        float dist2 = dot(toLight, toLight);
        float dist = sqrt(max(dist2, 1e-6));
        vec3 l = toLight / dist;

        float nDotL = max(dot(hit.shadingNormal, l), 0.0);
        float lightNdot = max(dot(lightNormal, -l), 0.0);
        if (nDotL <= 0.0 || lightNdot <= 0.0) {
            continue;
        }

        vec3 shadowOrigin = offsetRayOrigin(hit.position, hit.shadingNormal, l);
        if (occluded(shadowOrigin, l, dist)) {
            continue;
        }

        float lightPdf = dist2 / max(lightNdot * area, 1e-6);
        float diffusePdf = nDotL * (1.0 / kPi);
        float specPdf = ggxSpecularPdf(hit.shadingNormal, viewDir, l, roughness);
        float bsdfPdf = diffuseWeight * diffusePdf + specularWeight * specPdf;
        vec3 bsdf = diffuseBrdf;
        if (specularWeight > 1e-5) {
            float specBrdf = ggxSpecularBrdf(hit.shadingNormal, viewDir, l, roughness, f0);
            bsdf += vec3(specBrdf * specularWeight);
        }
        float misWeight = powerHeuristic(lightPdf, bsdfPdf);
        result += bsdf * lightRadiance * nDotL * misWeight / max(lightPdf, 1e-6);
    }

    return result / float(sampleCount);
}

PathResult tracePath(Ray ray, PathLdsSampler sampler) {
    PathResult outResult;
    outResult.radiance = vec3(0.0);
    outResult.primaryNormal = vec3(0.0, 1.0, 0.0);
    outResult.primaryAlbedo = vec3(0.0);
    outResult.hasPrimary = 0;

    vec3 throughput = vec3(1.0);
    int maxBounces = clamp(uParams.options.x, 1, 20);
    bool debugMono = (uParams.debug.x > 0.5);
    float debugLambdaNm = uParams.debug.y;
    bool prevHasBsdfSample = false;
    vec3 prevSamplePosition = vec3(0.0);
    float prevBsdfPdf = 0.0;
    bool mediumActive = false;
    vec3 mediumAbsorption = vec3(0.0);

    for (int bounce = 0; bounce < maxBounces; ++bounce) {
        Hit hit;
        if (!intersectScene(ray, 0.001, 1e20, hit)) {
            break;
        }
        if (hit.materialIndex < 0 || hit.materialIndex >= uParams.counts.w) {
            break;
        }

        GpuMaterial material = uMaterials.materials[hit.materialIndex];
        if (mediumActive && !debugMono) {
            throughput *= exp(-mediumAbsorption * hit.t);
        }
        if (bounce == 0) {
            outResult.primaryNormal = hit.shadingNormal;
            outResult.primaryAlbedo = material.albedoRoughness.rgb;
            outResult.hasPrimary = 1;
        }

        vec3 emission = material.emissionTransmission.rgb;
        if (max3(emission) > 1e-6) {
            float misWeight = 1.0;
            if (prevHasBsdfSample) {
                vec3 wi = hit.position - prevSamplePosition;
                float dist2 = dot(wi, wi);
                wi = normalize(wi);
                float area = max(uLight.colorSizeX.w * uLight.normalSizeY.w, 1e-4);
                float lightNdot = max(dot(normalize(uLight.normalSizeY.xyz), -wi), 0.0);
                if (lightNdot > 0.0) {
                    float lightPdf = dist2 / max(lightNdot * area, 1e-6);
                    misWeight = powerHeuristic(prevBsdfPdf, lightPdf);
                }
            }
            outResult.radiance += throughput * emission * uLight.centerIntensity.w * misWeight;
            break;
        }

        float roughness = clamp(material.albedoRoughness.w, 0.0, 0.8);
        float transmission = saturate(material.emissionTransmission.w);
        float diffuseEnabled = material.options.x;
        float iorBase = max(material.iorAbsorption.x, 1.0);
        float cosTheta = max(dot(-ray.dir, hit.shadingNormal), 0.0);
        float f0 = pow((iorBase - 1.0) / (iorBase + 1.0), 2.0);
        float fresnel = fresnelSchlick(cosTheta, f0);

        float reflectW = fresnel;
        float refractW = transmission * (1.0 - fresnel);
        float diffuseW = diffuseEnabled > 0.5 ? ((1.0 - transmission) * (1.0 - fresnel)) : 0.0;
        float sumW = max(refractW + reflectW + diffuseW, 1e-5);
        refractW /= sumW;
        reflectW /= sumW;
        diffuseW /= sumW;

        float directSpecularW = fresnel;
        vec3 viewDir = normalize(-ray.dir);
        outResult.radiance += throughput * sampleDirectLightNEE(
            hit, material, viewDir, diffuseW, directSpecularW, roughness, f0, sampler, bounce);

        uint dimBase = bounceDimBase(bounce);
        float branch = lds1D(sampler, dimBase + kDimBranch);
        vec3 nextDir = vec3(0.0);
        vec3 branchWeight = vec3(1.0);
        float branchProb = 1.0;
        bool nextHasBsdfSample = false;
        vec3 nextSamplePosition = vec3(0.0);
        float nextBsdfPdf = 0.0;
        bool nextMediumActive = mediumActive;
        vec3 nextMediumAbsorption = mediumAbsorption;
        const float kSmoothSpecThreshold = 0.05;
        float refractRoughness = clamp(roughness * 0.6, 0.0, 0.8);

        if (branch < refractW) {
            float lambdaPick = lds1D(sampler, dimBase + kDimWavelength);
            float lambda = debugMono ? debugLambdaNm : wavelengthNm(min(int(floor(lambdaPick * float(clamp(int(uParams.cauchy.z), 1, 6)))), 5));
            float lambdaUm = lambda * 0.001;
            float iorDispersion = uParams.cauchy.x + uParams.cauchy.y / (lambdaUm * lambdaUm);

            float eta = (hit.frontFace == 1) ? (1.0 / iorDispersion) : iorDispersion;
            vec3 n = hit.shadingNormal;
            vec3 refrDir = refract(ray.dir, n, eta);
            if (length(refrDir) < 1e-5) {
                if (roughness <= kSmoothSpecThreshold) {
                    nextDir = normalize(reflect(ray.dir, n));
                } else {
                    vec3 microN = sampleGGXNormal(n, roughness, lds2D(sampler, dimBase + kDimGgxRefract));
                    nextDir = normalize(reflect(ray.dir, microN));
                }
                branchProb = max(reflectW + refractW, 1e-4);
                branchWeight = vec3(1.0);
            } else {
                if (roughness <= kSmoothSpecThreshold) {
                    nextDir = normalize(refrDir);
                } else {
                    vec3 microN = sampleGGXNormal(n, refractRoughness, lds2D(sampler, dimBase + kDimGgxRefract));
                    vec3 roughRefrDir = refract(ray.dir, microN, eta);
                    if (length(roughRefrDir) < 1e-5) {
                        roughRefrDir = refrDir;
                    }
                    nextDir = normalize(roughRefrDir);
                }
                float transmissionTerm = max((1.0 - fresnel) * transmission, 1e-3);
                branchWeight = material.albedoRoughness.rgb * transmissionTerm;
                branchProb = max(refractW, 1e-4);
                if (hit.frontFace == 1) {
                    nextMediumActive = true;
                    nextMediumAbsorption = material.iorAbsorption.yzw;
                } else {
                    nextMediumActive = false;
                    nextMediumAbsorption = vec3(0.0);
                }
            }
        } else if (branch < refractW + reflectW) {
            if (roughness <= kSmoothSpecThreshold) {
                nextDir = normalize(reflect(ray.dir, hit.shadingNormal));
            } else {
                vec3 microN = sampleGGXNormal(hit.shadingNormal, roughness, lds2D(sampler, dimBase + kDimGgxReflect));
                nextDir = normalize(reflect(ray.dir, microN));
            }
            branchWeight = mix(material.albedoRoughness.rgb, vec3(1.0), 0.75);
            branchProb = max(reflectW, 1e-4);
            // Do not MIS-reweight emissive hits for specular reflection paths;
            // for near-delta lobes this can darken the reflected area light.
            nextHasBsdfSample = false;
            nextSamplePosition = vec3(0.0);
            nextBsdfPdf = 0.0;
        } else {
            nextDir = sampleHemisphere(hit.shadingNormal, lds2D(sampler, dimBase + kDimDiffuse));
            branchWeight = material.albedoRoughness.rgb;
            branchProb = max(diffuseW, 1e-4);
            nextHasBsdfSample = true;
            nextSamplePosition = hit.position;
            nextBsdfPdf = diffuseW * max(dot(hit.shadingNormal, nextDir), 0.0) * (1.0 / kPi);
        }

        // Russian Roulette for low-probability lobes to avoid huge 1/p amplification.
        const float kLowProbRRThreshold = 0.05;
        if (branchProb < kLowProbRRThreshold) {
            float survive = branchProb / kLowProbRRThreshold;
            if (lds1D(sampler, dimBase + kDimLowProbRr) > survive) {
                break;
            }
            branchProb = kLowProbRRThreshold;
        }

        throughput *= branchWeight / branchProb;
        prevHasBsdfSample = nextHasBsdfSample && (nextBsdfPdf > 1e-6);
        prevSamplePosition = nextSamplePosition;
        prevBsdfPdf = nextBsdfPdf;
        mediumActive = nextMediumActive;
        mediumAbsorption = nextMediumAbsorption;
        if (max3(throughput) < 1e-4) {
            break;
        }

        if (bounce >= 2) {
            float survive = clamp(max3(throughput), 0.05, 0.95);
            if (lds1D(sampler, dimBase + kDimThroughputRr) > survive) {
                break;
            }
            throughput /= survive;
        }

        ray.origin = offsetRayOrigin(hit.position, hit.shadingNormal, nextDir);
        ray.dir = nextDir;
    }

    return outResult;
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imageSizePx = imageSize(uAccumImage);
    if (pixel.x >= imageSizePx.x || pixel.y >= imageSizePx.y) {
        return;
    }

    vec3 currentColor = vec3(0.0);
    vec3 normalAccum = vec3(0.0);
    vec3 albedoAccum = vec3(0.0);
    int normalHits = 0;
    float lumMean = 0.0;
    float lumM2 = 0.0;
    int lumCount = 0;
    vec3 colorMean = vec3(0.0);
    vec3 colorM2 = vec3(0.0);
    int colorCount = 0;

    int spp = clamp(uParams.options.z, 1, 2048);
    uint frame = uint(uCamera.viewportAndFrame.z);
    uint pixelSeed = hash32(uint(pixel.x) * 1973u ^ uint(pixel.y) * 9277u ^ frame * 26699u ^ 97u);
    vec2 pixelShift = vec2(float(hash32(pixelSeed ^ 0x68bc21ebu)) / 4294967295.0,
                           float(hash32(pixelSeed ^ 0x02e5be93u)) / 4294967295.0);
    uint sppU = uint(spp);
    for (int sampleIndex = 0; sampleIndex < spp; ++sampleIndex) {
        vec2 jitter = fract(hammersley2D(uint(sampleIndex), sppU, pixelSeed) + pixelShift);
        vec2 uv = (vec2(pixel) + jitter) / vec2(float(imageSizePx.x), float(imageSizePx.y));
        vec2 ndc = uv * 2.0 - 1.0;

        vec4 clip = vec4(ndc, 1.0, 1.0);
        vec4 view = uCamera.invProj * clip;
        view /= max(view.w, 1e-6);
        vec4 world = uCamera.invView * vec4(view.xyz, 1.0);

        Ray primaryRay;
        primaryRay.origin = uCamera.cameraPosition.xyz;
        primaryRay.dir = normalize(world.xyz - primaryRay.origin);

        PathLdsSampler ldsSampler;
        ldsSampler.pixelSeed = pixelSeed;
        ldsSampler.sampleIndex = uint(sampleIndex);
        ldsSampler.frameIndex = frame;

        PathResult sampleResult = tracePath(primaryRay, ldsSampler);
        vec3 sampleRadiance = sampleResult.radiance;

        // Robust/adaptive firefly clamp using running luminance statistics.
        float rawLum = luminance(sampleRadiance);
        float statsLum = min(rawLum, 128.0);
        lumCount += 1;
        float delta = statsLum - lumMean;
        lumMean += delta / float(lumCount);
        float delta2 = statsLum - lumMean;
        lumM2 += delta * delta2;

        float maxSampleLum = 32.0;
        if (lumCount >= 4) {
            float variance = lumM2 / max(float(lumCount - 1), 1.0);
            float sigma = sqrt(max(variance, 1e-6));
            maxSampleLum = clamp(lumMean + 4.0 * sigma, 4.0, 64.0);
        }
        vec3 statsColor = clamp(sampleRadiance, vec3(0.0), vec3(128.0));
        colorCount += 1;
        vec3 cDelta = statsColor - colorMean;
        colorMean += cDelta / float(colorCount);
        vec3 cDelta2 = statsColor - colorMean;
        colorM2 += cDelta * cDelta2;

        vec3 maxSampleColor = vec3(32.0);
        if (colorCount >= 4) {
            vec3 cVariance = colorM2 / max(float(colorCount - 1), 1.0);
            vec3 cSigma = sqrt(max(cVariance, vec3(1e-6)));
            maxSampleColor = clamp(colorMean + 4.0 * cSigma, vec3(2.0), vec3(64.0));
        }
        sampleRadiance = clamp(sampleRadiance, vec3(0.0), maxSampleColor);
        sampleRadiance = clampLuminance(sampleRadiance, maxSampleLum);

        currentColor += sampleRadiance;
        if (sampleResult.hasPrimary == 1) {
            normalAccum += sampleResult.primaryNormal;
            albedoAccum += sampleResult.primaryAlbedo;
            normalHits += 1;
        }
    }

    currentColor /= float(spp);
    vec3 avgNormal = normalHits > 0 ? normalize(normalAccum / float(normalHits)) : vec3(0.0, 1.0, 0.0);
    vec3 avgAlbedo = normalHits > 0 ? (albedoAccum / float(normalHits)) : vec3(0.0);

    vec4 previousAccum = imageLoad(uAccumImage, pixel);
    vec3 previousNormal = normalize(imageLoad(uNormalImage, pixel).rgb * 2.0 - 1.0);
    vec3 previousAlbedo = imageLoad(uAlbedoImage, pixel).rgb;
    float alpha = (uParams.options.w == 1) ? 1.0 : clamp(uParams.alphaAndMode.x, 0.0, 1.0);
    vec4 current = vec4(currentColor, 1.0);
    vec4 accum = alpha * current + (1.0 - alpha) * previousAccum;
    vec3 accumNormal = normalize(alpha * avgNormal + (1.0 - alpha) * previousNormal);
    vec3 accumAlbedo = alpha * avgAlbedo + (1.0 - alpha) * previousAlbedo;

    imageStore(uAccumImage, pixel, accum);
    imageStore(uOutputImage, pixel, accum);
    imageStore(uBeautyImage, pixel, vec4(currentColor, 1.0));
    imageStore(uNormalImage, pixel, vec4(accumNormal * 0.5 + 0.5, 1.0));
    imageStore(uAlbedoImage, pixel, vec4(accumAlbedo, 1.0));
}

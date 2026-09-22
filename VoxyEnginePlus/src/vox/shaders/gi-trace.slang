#include "atmosphere.slang"
#include "primary-ray.slang"
#include "voxel-hashmap.slang"

// Path-traced indirect lighting, at LIGHTING_DOWNSAMPLE resolution.
//
// This used to live inside the full-res gbuffer pass, guarded by `pixel % 2 == 0`. That was
// expensive in a way the guard hides: the bounce code's register footprint applies to the whole
// shader, so the three quarters of lanes that skipped the work still paid for it in lost occupancy.
// Measured on an RTX 4070 laptop, deleting the block entirely cut the gbuffer pass from 4.6 ms to
// 1.2 ms while deleting any individual *part* of it changed almost nothing - the classic signature
// of an occupancy wall rather than an instruction-count problem.
//
// Running it as its own half-res dispatch gives the bounce code its own register budget and leaves
// the gbuffer pass lean. It costs one extra primary trace per GI pixel (a quarter of the screen),
// which is far cheaper than what the occupancy wall was costing.
//
// It is scheduled *before* the gbuffer pass on purpose: the reprojection below wants last frame's
// position buffer, which is exactly what it reads there.

static const float3 UP_VECTOR = float3(0, 1, 0);
static const float3 FORWARD_VECTOR = float3(0, 0, 1);
static const float PARALLEL_THRESHOLD = 0.9999f;
static const float TEMPORAL_MIN_ALPHA = 0.4;

// Creates a 3x3 rotation matrix that rotates from up vector to the given normal
float3x3 CreateRotationFromNormal3x3(float3 normal)
{
    // Ensure normal is normalized
    normal = normalize(normal);

    // Check if normal is nearly parallel to up vector
    float upDot = dot(normal, UP_VECTOR);

    float3 right;
    if (abs(upDot) > PARALLEL_THRESHOLD)
    {
        // If parallel, use forward vector as reference
        right = normalize(cross(FORWARD_VECTOR, normal));
    }
    else
    {
        right = normalize(cross(UP_VECTOR, normal));
    }

    // Create forward vector to complete orthonormal basis
    float3 forward = normalize(cross(normal, right));

    // Construct rotation matrix
    // Each column represents one of our basis vectors
    return float3x3(
        right.x, normal.x, forward.x,
        right.y, normal.y, forward.y,
        right.z, normal.z, forward.z);
}

float3 transformBlueNoise(float3 sample, uint frameIndex)
{
    // Simple Cranley-Patterson rotation using frame index
    float3 shift = float3(
        frac(frameIndex * 0.754877669), // Using golden ratio conjugate
        frac(frameIndex * 0.569840291),
        frac(frameIndex * 0.682327803));
    return frac(sample + shift);
}

TraceResult computeBounce(int2 pixel, float3 origin, float3 normal, float3 blueNoiseSample, uint frameIndex, out float3 rayDir)
{
    float3 hitPos = origin + normal * EPSILON;
    float3x3 basis = CreateRotationFromNormal3x3(normal);
    float3 blue = blueNoiseSample;
    blue = transformBlueNoise(blue, p.state_ptr.frame + uint(pixel.x * p.frame_dim.y + pixel.y));
    blue = blue * 2 - 1;
    blue.y = abs(blue.y);
    blue = mul(basis, blue);
    rayDir = normalize(blue);

    return traceVoxelRay(hitPos, rayDir, 0, 512);
}

Material getMaterial(uint voxelIndex)
{
    return p.materials_ptr.materials[voxelIndex];
}

/// Material at a bounce hit. On a miss the result is unused by calculateBounceColor, but the old
/// code still issued the lookup - a scattered read into the ~1.8 GiB voxel material buffer for
/// every ray that escaped to the sky.
Material getBounceMaterial(TraceResult bounce)
{
    if (!bounce.hit)
        return (Material)0;

    return getMaterial(getMaterialId(int2(brickGlobalIndex(bounce.chunkIndex, bounce.brickIndex), bounce.voxelIndex)));
}

uint getMaterialId(int2 voxelID)
{
    uint globalVoxelIndex = voxelGlobalIndex(voxelID.x, voxelID.y);
    return (p.voxel_materials_ptr.materials[globalVoxelIndex / 8] >> ((globalVoxelIndex % 8) * 4)) & 0xF;
}

float3 calculateBounceColor(TraceResult bounceResult, Material hitMat, float3 rayDir)
{
    if (!bounceResult.hit)
    {
        AtmosphereParams atmosphere = initAtmosphere(rayDir, p.state_ptr.sunDir);
        return getBaseSkyColor(atmosphere) * 4;
    }
    else
    {
        bool sunBlocked = traceVoxelRayAnyHit(bounceResult.hitPos + bounceResult.normal * EPSILON, p.state_ptr.sunDir);

        // get the hashmap entry for the hit voxel and add the light from it if it exists
        float3 passiveBounce = float3(0, 0, 0);
        uint slot = get_hashtable_index(p.voxel_hashmap_ptr, brickGlobalIndex(bounceResult.chunkIndex, bounceResult.brickIndex), getHashtableFaceIndex(bounceResult));
        if (slot != EMPTY_KEY)
        {
            var entry = p.voxel_hashmap_ptr.entries[slot];
            if (entry.historyCount > 4)
            {
                passiveBounce = entry.lighting * 0.5;
            }
        }

        if (!sunBlocked)
        {
            // traceVoxelRay only ever assigns `normal` at a hit, so a miss always carried
            // float3(0,0,0) here. Kept literal so the sun term is bit-for-bit what it was.
            return hitMat.emission + passiveBounce + getSunColor(initAtmosphere(float3(0, 0, 0), p.state_ptr.sunDir)) * 5 * hitMat.albedo;
        }

        return hitMat.emission + passiveBounce;
    }
}

bool validateReprojection(float3 currentPosition, float3 previousPosition)
{
    float xDiff = abs(currentPosition.x - previousPosition.x);
    float yDiff = abs(currentPosition.y - previousPosition.y);
    float zDiff = abs(currentPosition.z - previousPosition.z);

    return xDiff < WORLD_VOXEL_SIZE || yDiff < WORLD_VOXEL_SIZE || zDiff < WORLD_VOXEL_SIZE;
}

void getBilinearWeights(float2 pos, out int2 baseCoord, out float2 weights)
{
    baseCoord = int2(floor(pos));
    weights = frac(pos);
}

float4 sampleHistoryBilinear(RWTexture2D<float4> history, float2 pos, float2 dimensions)
{
    int2 baseCoord;
    float2 weights;
    getBilinearWeights(pos, baseCoord, weights);

    // Clamp coordinates to texture bounds
    int2 c00 = clamp(baseCoord, int2(0, 0), int2(dimensions) - 1);
    int2 c10 = clamp(int2(baseCoord.x + 1, baseCoord.y), int2(0, 0), int2(dimensions) - 1);
    int2 c01 = clamp(int2(baseCoord.x, baseCoord.y + 1), int2(0, 0), int2(dimensions) - 1);
    int2 c11 = clamp(baseCoord + int2(1, 1), int2(0, 0), int2(dimensions) - 1);

    // Sample four nearest pixels
    float4 s00 = history[c00];
    float4 s10 = history[c10];
    float4 s01 = history[c01];
    float4 s11 = history[c11];

    // Bilinear blend
    float4 s0 = lerp(s00, s10, weights.x);
    float4 s1 = lerp(s01, s11, weights.x);
    return lerp(s0, s1, weights.y);
}

[numthreads(GI_TRACE_GROUP, GI_TRACE_GROUP, 1)]
void main(uint3 thread_id: SV_DispatchThreadID)
{
    uint2 samplePixel = thread_id.xy;
    float2 dimensions = p.frame_dim / LIGHTING_DOWNSAMPLE;
    if (samplePixel.x >= (uint)dimensions.x || samplePixel.y >= (uint)dimensions.y)
        return;

    // The full-res pixel this GI sample stands for - the same one the gbuffer pass will shade.
    uint2 pixel = samplePixel * LIGHTING_DOWNSAMPLE;

    RWTexture2D<float3> position = RWTexture2D<float3>::get(p.gbuffer.position);
    RWTexture2D<float4> indirect = RWTexture2D<float4>::get(p.gbuffer.indirect);
    RWTexture2D<float4> indirectLast = RWTexture2D<float4>::get(p.gbuffer.indirectLast);
    RWTexture2D<float4> blueNoise = RWTexture2D<float4>::get(p.blueNoise);

    TraceResult trace = tracePrimaryRay(pixel);
    if (!trace.hit)
        return;

    // Reprojection into last frame's indirect buffer.
    float2 motionClip = calculateMotionClip(trace.hitPos, p.state_ptr.camera.viewProj, p.state_ptr.lastCamera.viewProj);
    float2 motionPixels = motionClip * (p.frame_dim * 0.5);
    if (length(motionPixels) < 0.0001 || isnan(motionPixels.x) || isnan(motionPixels.y))
    {
        motionPixels = float2(0, 0);
    }

    float3 bounceRayDir;
    float4 blueNoiseSample = blueNoise[samplePixel % 128];
    TraceResult bounceResult = computeBounce(pixel, trace.hitPos, trace.normal, blueNoiseSample.xyz, p.state_ptr.frame, bounceRayDir);
    Material hitMat = getBounceMaterial(bounceResult);
    float3 rayBounceColor = calculateBounceColor(bounceResult, hitMat, bounceRayDir);

    if (bounceResult.hit && all(pixel % (LIGHTING_DOWNSAMPLE * 2) == 0))
    {
        float4 blueNoiseSample2 = blueNoise[(samplePixel + 17) % 128];
        TraceResult bounceResult2 = computeBounce(pixel, bounceResult.hitPos, bounceResult.normal, blueNoiseSample2.xyz, p.state_ptr.frame, bounceRayDir);
        Material hitMat2 = getBounceMaterial(bounceResult2);
        rayBounceColor = lerp(rayBounceColor, calculateBounceColor(bounceResult2, hitMat2, bounceRayDir) * pow(LIGHTING_DOWNSAMPLE, 2), 0.5);
    }

    // Calculate previous position with subpixel precision
    float2 prevPos = (float2(pixel) + motionPixels) * LIGHTING_UPSAMPLE;
    int2 prevPixel = int2(prevPos);
    // Validate history against last frame's position buffer (this pass runs before the gbuffer
    // pass overwrites it, which is what makes the lookup a true history lookup).
    float valid = validateReprojection(position[pixel].xyz, position[prevPixel * LIGHTING_DOWNSAMPLE].xyz);

    prevPos = clamp(prevPos, float2(0), dimensions - 1);

    // Sample lastFrame with bilinear interpolation
    float4 last = sampleHistoryBilinear(indirectLast, prevPos, dimensions);
    last = clamp(last, float4(0), float4(10000));

    if (!valid)
    {
        last.w = 0;
    }

    // do extra bounces for new pixels
    if (last.w < 2)
    {
        float4 blueNoiseSample3 = blueNoise[(samplePixel + 46) % 128];
        TraceResult bounceResult3 = computeBounce(pixel, trace.hitPos, trace.normal, blueNoiseSample3.xyz, p.state_ptr.frame, bounceRayDir);
        Material hitMat3 = getBounceMaterial(bounceResult3);
        rayBounceColor = lerp(rayBounceColor, calculateBounceColor(bounceResult3, hitMat3, bounceRayDir), 0.5);
    }

    float blendFactor = (1 / (last.w + 1));
    blendFactor = clamp(blendFactor, TEMPORAL_MIN_ALPHA, 1.0);

    // Final blend
    float3 blended = lerp(last.rgb, rayBounceColor, blendFactor);
    indirect[samplePixel] = float4(blended, last.w + 1);
}

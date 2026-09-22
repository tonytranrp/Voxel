#include "atmosphere.slang"
#include "primary-ray.slang"
#include "voxel-hashmap.slang"

// Full-res primary visibility + direct shading into the gbuffer.
//
// The indirect-lighting bounce code used to live here behind a `pixel % 2 == 0` guard; it now runs
// as its own half-res dispatch (gi-trace.slang) because its register footprint was throttling
// occupancy for every lane in this shader, not just the quarter that executed it.

Material getMaterial(uint voxelIndex)
{
    return p.materials_ptr.materials[voxelIndex];
}

uint getMaterialId(int2 voxelID)
{
    uint globalVoxelIndex = voxelGlobalIndex(voxelID.x, voxelID.y);
    return (p.voxel_materials_ptr.materials[globalVoxelIndex / 8] >> ((globalVoxelIndex % 8) * 4)) & 0xF;
}

[numthreads(GBUFFER_GROUP, GBUFFER_GROUP, 1)]
void main(uint3 pixel_i: SV_DispatchThreadID)
{
    uint2 pixel = pixel_i.xy;
    if (pixel.x >= p.frame_dim.x || pixel.y >= p.frame_dim.y)
        return;

    // gbuffer textures
    RWTexture2D<float3> albedo = RWTexture2D<float3>::get(p.gbuffer.color);
    RWTexture2D<int3> normal = RWTexture2D<int3>::get(p.gbuffer.normal);
    RWTexture2D<float3> position = RWTexture2D<float3>::get(p.gbuffer.position);
    RWTexture2D<float2> voxelUVs = RWTexture2D<float2>::get(p.gbuffer.voxelUVs);
    RWTexture2D<float2> brickUVs = RWTexture2D<float2>::get(p.gbuffer.brickUVs);
    RWTexture2D<float2> motion = RWTexture2D<float2>::get(p.gbuffer.motion);
    RWTexture2D<float> depth = RWTexture2D<float>::get(p.gbuffer.depth);
    RWTexture2D<int2> voxelIDs = RWTexture2D<int2>::get(p.gbuffer.voxelIDs);
    RWTexture2D<uint> voxelFaceIDs = RWTexture2D<uint>::get(p.gbuffer.voxelFaceIDs);
    RWTexture2D<uint> materialIDs = RWTexture2D<uint>::get(p.gbuffer.materialIDs);
    RWTexture2D<float> shadowTex = RWTexture2D<float>::get(p.gbuffer.shadow);

    // The GI passes run at half res and only ever look at pixels on that grid.
    const bool isGiPixel = all(pixel % LIGHTING_DOWNSAMPLE == 0);
    RWTexture2D<uint4> giIDs = RWTexture2D<uint4>::get(p.gbuffer.giIDs);

    // trace main ray (beam-optimised start depth comes from the half-res prepass)
    TraceResult trace = tracePrimaryRay(pixel);
    if (!trace.hit)
    {
        depth[pixel] = 10000.0;
        voxelIDs[pixel] = int2(EMPTY_KEY, EMPTY_KEY);

        if (isGiPixel)
        {
            // voxelFaceIDs is deliberately not written on this path, so its half-res mirror has to
            // keep the previous frame's face id - hence read-modify-write rather than a plain store.
            uint2 hp = pixel / LIGHTING_DOWNSAMPLE;
            giIDs[hp] = uint4(EMPTY_KEY, EMPTY_KEY, giIDs[hp].z, 0);
        }
        return;
    }

    // get hit material
    int globalBrickIndex = brickGlobalIndex(trace.chunkIndex, trace.brickIndex);
    uint voxelMaterialId = getMaterialId(int2(globalBrickIndex, trace.voxelIndex));
    Material traceMat = getMaterial(voxelMaterialId);

    // get motion vectors
    float2 motionClip = calculateMotionClip(trace.hitPos, p.state_ptr.camera.viewProj, p.state_ptr.lastCamera.viewProj);
    float2 motionPixels = motionClip * (p.frame_dim * 0.5);
    if (length(motionPixels) < 0.0001 || isnan(motionPixels.x) || isnan(motionPixels.y))
    {
        motionPixels = float2(0, 0);
    }

    // trace hard shadow ray (visibility only - the hit point itself is never used)
    float shadow = traceVoxelRayAnyHit(trace.hitPos + trace.normal * EPSILON, normalize(p.state_ptr.sunDir)) ? 1 : 0;

    float3 albedoColor = traceMat.albedo + traceMat.emission;

    float2 voxelFaceUV = trace.normal.x != 0 ? trace.voxelUv.yz : trace.normal.y != 0 ? trace.voxelUv.xz
                                                                                      : trace.voxelUv.xy;
    float2 brickFaceUV = trace.normal.x != 0 ? trace.brickUv.yz : trace.normal.y != 0 ? trace.brickUv.xz
                                                                                      : trace.brickUv.xy;

    // write to the gbuffer
    int3 packedNormal = int3(normalize(trace.normal));
    albedo[pixel] = albedoColor;
    normal[pixel] = packedNormal;
    position[pixel] = trace.hitPos;

    // Mirror position+normal into the half-res GI geometry target for the pixels the denoiser
    // samples. Written at exactly the same point as position/normal above, so it carries the same
    // values (including the "kept from last frame on a sky pixel" behaviour of the early-out).
    if (isGiPixel)
    {
        RWTexture2D<float4> giGeometry = RWTexture2D<float4>::get(p.gbuffer.giGeometry);
        giGeometry[pixel / LIGHTING_DOWNSAMPLE] = float4(trace.hitPos, packNormalAxis(packedNormal));
    }
    voxelUVs[pixel] = voxelFaceUV;
    brickUVs[pixel] = brickFaceUV;
    depth[pixel] = trace.distance;
    motion[pixel] = trace.hit ? motionPixels : float2(0, 0);
    shadowTex[pixel] = 1 - shadow;
    materialIDs[pixel] = voxelMaterialId;

    // trace.hit is necessarily true here - the miss case returned above.
    const uint faceID = getHashtableFaceIndex(trace);
    voxelIDs[pixel] = int2(globalBrickIndex, trace.voxelIndex);
    voxelFaceIDs[pixel] = faceID;

    if (isGiPixel)
    {
        giIDs[pixel / LIGHTING_DOWNSAMPLE] = uint4(uint(globalBrickIndex), uint(trace.voxelIndex), faceID, 0);
    }
}

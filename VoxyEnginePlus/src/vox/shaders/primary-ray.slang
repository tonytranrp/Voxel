#pragma once

#include "raycast.slang"

// Shared front end for the two passes that shoot camera rays: the full-res gbuffer pass and the
// half-res GI pass. Keeping it in one place means both traces start from identical state, so the
// GI pass sees exactly the surface the gbuffer pass records for the same pixel.

bool IsObjectSmallerThanPixel(
    float objectWorldSize,  // Size of object in world units
    float distanceToCamera, // Distance from camera to object in world units
    float horizontalFOV,    // Camera's horizontal FOV in radians
    uint screenWidth)       // Screen width in pixels
{
    // Calculate how many radians one pixel represents
    float radiansPerPixel = horizontalFOV / screenWidth;
    float objectAngularSize = objectWorldSize / distanceToCamera;

    // angular size is smaller than one pixel
    return objectAngularSize < radiansPerPixel;
}

uint getHashtableFaceIndex(TraceResult trace)
{
    bool isVoxelFarLOD = IsObjectSmallerThanPixel(
        WORLD_VOXEL_SIZE * pow(LIGHTING_UPSAMPLE, 2),
        trace.distance,
        p.state_ptr.camera.fov * 0.0174532925f,
        p.frame_dim.x);

    uint voxelIndex = isVoxelFarLOD ? -1 : ((trace.voxelIndex + 1));
    uint normalIndex = (trace.normal.x != 0 ? 1 : 0) + (trace.normal.y != 0 ? 2 : 0) + (trace.normal.z != 0 ? 4 : 0);
    // voxelIndex += 216 * normalIndex;
    return voxelIndex;
}

/// Distance the camera ray can safely skip before it has to start stepping voxels, taken from the
/// half-res depth prepass (the "beam optimization"). Conservative: it takes the minimum over the
/// 3x3 prepass neighbourhood so a ray never starts past geometry a neighbouring beam saw.
float primaryRayStartDepth(uint2 pixel)
{
    if (!(bool)p.state_ptr.settings.beamOptimization)
        return 0.0;

    RWTexture2D<float> depthHalfRes = RWTexture2D<float>::get(p.gbuffer.depthHalfRes);

    uint2 prepassPixel = pixel / 2;
    float startDepth = depthHalfRes[prepassPixel];

    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            uint2 checkPixel = uint2(
                clamp(int(prepassPixel.x) + dx, 0, int(p.frame_dim.x / 2) - 1),
                clamp(int(prepassPixel.y) + dy, 0, int(p.frame_dim.y / 2) - 1));
            startDepth = min(startDepth, depthHalfRes[checkPixel]) - WORLD_VOXEL_SIZE * 0.5;
        }
    }

    return startDepth;
}

/// The camera ray for a full-res pixel, including the beam-optimization head start.
TraceResult tracePrimaryRay(uint2 pixel)
{
    float3 rayOrigin, rayDir;
    getRayFromPixel(pixel, p.frame_dim, p.state_ptr.camera, rayOrigin, rayDir);
    return traceVoxelRay(rayOrigin, rayDir, primaryRayStartDepth(pixel));
}

/// Screen-space motion of a world point between last frame and this one, in clip units.
float2 calculateMotionClip(float3 worldPos, float4x4 viewProj, float4x4 lastViewProj)
{
    float4 currentClip = mul(viewProj, float4(worldPos, 1.0));
    float4 previousClip = mul(lastViewProj, float4(worldPos, 1.0));

    // Get clip space positions after perspective divide
    float2 currentPos = currentClip.xy / currentClip.w;
    float2 previousPos = previousClip.xy / previousClip.w;

    // Return raw clip space motion
    return previousPos - currentPos;
}

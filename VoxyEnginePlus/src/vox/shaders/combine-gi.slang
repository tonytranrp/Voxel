#include "shared.inl"
#include "voxel-hashmap.slang"

[[vk::push_constant]]
const ComputePush p;

// Averages the denoised indirect light across each voxel face: a horizontal run pass, a vertical
// run pass, then a write-back into the per-voxel hashmap.
//
// Every comparison here used to fetch voxelIDs and voxelFaceIDs at `halfResPixel * 2`, striding
// through two full-res targets - and the run walks do it once per step, so a long run re-read them
// dozens of times. giIDs is a half-res mirror of exactly those values written by the gbuffer pass,
// so one coherent fetch now replaces two scattered ones.
[numthreads(COMBINE_GROUP, COMBINE_GROUP, 1)]
void main(uint3 pixel_i: SV_DispatchThreadID)
{
    RWTexture2D<float3> indirect = RWTexture2D<float3>::get(p.gbuffer.indirectDenoised);
    RWTexture2D<float3> indirectPerVoxPass1 = RWTexture2D<float3>::get(p.gbuffer.indirectPerVoxelPass1);
    RWTexture2D<float3> indirectPerVoxPass2 = RWTexture2D<float3>::get(p.gbuffer.indirectPerVoxelPass2);

    RWTexture2D<uint4> giIDs = RWTexture2D<uint4>::get(p.gbuffer.giIDs);

    uint2 pixel = pixel_i.xy;
    uint4 ids = giIDs[pixel];
    int2 voxelID = int2(ids.xy);
    uint voxelFaceID = ids.z;

    if (p.pass == 1)
    {
        float3 avgColor = indirect[pixel];

        int avgCount = 1;
        uint4 offsetIDs = giIDs[pixel + int2(-1, 0)];
        if (int(offsetIDs.x) != voxelID.x || offsetIDs.z != voxelFaceID)
        {
            offsetIDs = giIDs[pixel + int2(avgCount, 0)];
            while (int(offsetIDs.x) == voxelID.x && offsetIDs.z == voxelFaceID && avgCount < 1000)
            {
                float3 sampleColor = indirect[(pixel + int2(avgCount, 0))];
                avgColor += sampleColor;

                avgCount++;
                offsetIDs = giIDs[pixel + int2(avgCount, 0)];
            }

            for (int i = 0; i < avgCount; i++)
            {
                indirectPerVoxPass1[(pixel + int2(i, 0))] = avgColor / avgCount;
            }
        }
    }

    if (p.pass == 2)
    {
        float3 avgColor = indirectPerVoxPass1[pixel];

        int avgCount = 1;
        uint4 offsetIDs = giIDs[pixel + int2(0, -1)];
        if (int(offsetIDs.x) != voxelID.x || offsetIDs.z != voxelFaceID)
        {
            offsetIDs = giIDs[pixel + int2(0, avgCount)];
            while (int(offsetIDs.x) == voxelID.x && offsetIDs.z == voxelFaceID && avgCount < 1000)
            {
                float3 sampleColor = indirectPerVoxPass1[(pixel + int2(0, avgCount))];
                avgColor += sampleColor;

                avgCount++;
                offsetIDs = giIDs[pixel + int2(0, avgCount)];
            }

            for (int i = 0; i < avgCount; i++)
            {
                indirectPerVoxPass2[(pixel + int2(0, i))] = avgColor / avgCount;
            }
        }
    }

    if (p.pass == 3)
    {
        uint4 offsetIDs = giIDs[pixel + int2(-1, -1)];
        uint4 offsetIDs2 = giIDs[pixel + int2(1, 1)];
        int2 offsetVoxID = int2(offsetIDs.xy);
        int2 offsetVoxID2 = int2(offsetIDs2.xy);

        if (any(offsetVoxID != voxelID) || any(offsetVoxID2 != voxelID) ||
            offsetIDs.z != voxelFaceID || offsetIDs2.z != voxelFaceID)
        {
            float3 finalColor = indirectPerVoxPass2[pixel];

            uint slot = get_hashtable_index(p.voxel_hashmap_ptr, voxelID.x, voxelFaceID);
            uint lastTempFrames = 0;
            float3 lastColor = float3(0, 0, 0);

            if (slot != EMPTY_KEY)
            {
                var entry = p.voxel_hashmap_ptr.entries[slot];
                lastTempFrames = entry.historyCount;
                lastColor = entry.lighting;
            }

            float blendFactor = 1.0 / (lastTempFrames + 1);
            blendFactor = clamp(blendFactor, 0.01, 0.99);
            finalColor = lastColor * (1 - blendFactor) + finalColor * (blendFactor);

            voxel_hashtable_insert(p.voxel_hashmap_ptr, voxelID.x, voxelFaceID, finalColor, lastTempFrames + 1, p.state_ptr.frame);
        }
    }
}

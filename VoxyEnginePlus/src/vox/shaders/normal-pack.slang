#pragma once

// Voxel face normals are always axis aligned, so each component is exactly -1, 0 or 1. Written as
// balanced ternary that fits exactly in one float channel, which lets position and normal share a
// single RGBA32F texel instead of living in two separately sampled render targets.
//
// The encoding deliberately maps the zero normal to 0.0, so a texel that has never been written
// decodes to float3(0,0,0) - the same "no geometry here" value a cleared normal target gives.
float packNormalAxis(int3 n)
{
    return float(n.x + n.y * 3 + n.z * 9);
}

float3 unpackNormalAxis(float packed)
{
    int t = int(packed) + 13; // shift balanced ternary [-13,13] into [0,26]
    int x = t % 3;
    t /= 3;
    int y = t % 3;
    t /= 3;
    return float3(float(x - 1), float(y - 1), float(t - 1));
}

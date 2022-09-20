// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

cbuffer ConstBuffer : register(b0)
{
};

Texture2D<float4> glyphAtlas : register(t0);

// clang-format off
float4 main(
    float4 position : POSITION,
    float2 texCoord : TEXCOORD,
    float4 color : COLOR
) : SV_Target
// clang-format on
{
    return glyphAtlas[texCoord] * color;
}

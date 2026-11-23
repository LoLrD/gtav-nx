#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#define g_Booleans                 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 512)
#define g_SwappedTexcoords         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 516)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 520)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 528)
#define g_AlphaToMask              vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 532)
#define g_NdcScale                 vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 536)
#define g_NdcOffset                vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 544)
#define g_AlphaFunc                vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 552)
#define g_ColorScale(INDEX)        vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 560 + (INDEX) * 16)

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;
// Host pixels per guest pixel, inverted (render scale below 1): vPos stays in guest pixels.
[[vk::constant_id(1)]] const float g_PixelScaleInv = 1.0;

#define g_SpecConstants() g_SpecConstants

#else

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c32.x); \
    uint g_SwappedTexcoords : packoffset(c32.y); \
    float2 g_HalfPixelOffset : packoffset(c32.z); \
    float g_AlphaThreshold : packoffset(c33.x);     uint g_AlphaToMask : packoffset(c33.y); \
    float2 g_NdcScale : packoffset(c33.z); \
    float2 g_NdcOffset : packoffset(c34.x); \
    uint g_AlphaFunc : packoffset(c34.z); \
    float4 g_ColorScaleArray[4] : packoffset(c35);

uint g_SpecConstants();

#define g_PixelScaleInv 1.0

#define g_ColorScale(INDEX) g_ColorScaleArray[INDEX]

#endif

// Vertex shaders have no derivatives: sample mip 0 there, like Xenos vertex texture fetch.
#if defined(__SHADER_TARGET_STAGE) && __SHADER_TARGET_STAGE == __SHADER_STAGE_VERTEX
#define XENOS_SAMPLE(texture, samplerState, texCoord) (texture).SampleLevel(samplerState, texCoord, 0)
#else
#define XENOS_SAMPLE(texture, samplerState, texCoord) (texture).Sample(samplerState, texCoord)
#endif

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

// GTA V: like the SDK's shader translator (kRoundingOffset), every fetch gets 1.5/1024 texel added
// to its offset. With D3D9 pixel centres, screen-space passes put coordinates exactly on texel
// edges, and point sampling then picks texel i or i-1 depending on interpolation rounding (the
// ocean wave simulation became a blend of neighbours and drifted).
#define XENOS_FETCH_ROUNDING (1.5 / 1024.0)

// 1D textures are bound as 2D textures of height 1.
float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return XENOS_SAMPLE(texture, g_SamplerDescriptorHeap[samplerDescriptorIndex],
                        float2(texCoord + XENOS_FETCH_ROUNDING / getTexture2DDimensions(texture).x, 0.5));
}

float getWeights1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture).x + XENOS_FETCH_ROUNDING - 0.5));
}

// GTA V: fetches with unnormalized coordinates (texCoordDenorm) address texels of the base level.
float denorm1D(uint resourceDescriptorIndex, float texCoord)
{
    return texCoord / getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]).x;
}

float2 denorm2D(uint resourceDescriptorIndex, float2 texCoord)
{
    return texCoord / getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]);
}

float3 denorm3D(uint resourceDescriptorIndex, float3 texCoord)
{
    uint w, h, d;
    g_Texture3DDescriptorHeap[resourceDescriptorIndex].GetDimensions(w, h, d);
    return texCoord / float3(w, h, d);
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return XENOS_SAMPLE(texture, g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + (offset + XENOS_FETCH_ROUNDING) / getTexture2DDimensions(texture));
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset + XENOS_FETCH_ROUNDING - 0.5));
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * XENOS_SAMPLE(texture, samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * XENOS_SAMPLE(texture, samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * XENOS_SAMPLE(texture, samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * XENOS_SAMPLE(texture, samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float3 roundingOffset3D(uint resourceDescriptorIndex)
{
    uint w, h, d;
    g_Texture3DDescriptorHeap[resourceDescriptorIndex].GetDimensions(w, h, d);
    return XENOS_FETCH_ROUNDING / float3(w, h, d);
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return XENOS_SAMPLE(g_Texture3DDescriptorHeap[resourceDescriptorIndex], g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + roundingOffset3D(resourceDescriptorIndex));
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return XENOS_SAMPLE(g_TextureCubeDescriptorHeap[resourceDescriptorIndex], g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

// GTA V: fetches with an explicit LOD (setTexLOD + UseRegisterLOD, or computed LOD disabled).
float4 tfetch1DLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord, float lod)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex],
                               float2(texCoord + XENOS_FETCH_ROUNDING / getTexture2DDimensions(texture).x, 0.5), lod);
}

float4 tfetch2DLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float lod)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + (offset + XENOS_FETCH_ROUNDING) / getTexture2DDimensions(texture), lod);
}

// GTA V: fetches whose instruction overrides the filter to point (mag and min) read one texel, as
// the hardware does; the descriptor's sampler is the fetch constant's (usually linear), which
// blended depths across edges in min/max depth downsamples (tiled lighting data).
float4 tfetch2DPoint(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    int2 dims = int2(getTexture2DDimensions(texture));
    int2 p = clamp(int2(floor(texCoord * float2(dims) + offset + XENOS_FETCH_ROUNDING)), int2(0, 0), dims - 1);
    return texture.Load(int3(p, 0));
}

float4 tfetch2DLodPoint(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float lod)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    uint w, h, levels;
    texture.GetDimensions(0, w, h, levels);
    uint mip = uint(clamp(round(lod), 0.0, float(levels - 1)));
    texture.GetDimensions(mip, w, h, levels);
    int2 dims = int2(w, h);
    int2 p = clamp(int2(floor(texCoord * float2(dims) + offset + XENOS_FETCH_ROUNDING)), int2(0, 0), dims - 1);
    return texture.Load(int3(p, mip));
}

float4 tfetch3DLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float lod)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + roundingOffset3D(resourceDescriptorIndex), lod);
}

float4 tfetchCubeLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData, float lod)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z], lod);
}

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}

#ifdef __spirv__

// GTA V: vertex fetches with a computed index (tiled lighting, skinning palettes) read the guest
// vertex buffer directly. Per input location the host provides a 32-byte record at
// SharedConstants + 768 + location * 32: the stream's raw guest data address (with the fetch
// constant's endian mode in the low two bits), the stride and the element offset in dwords, and
// the element's D3DDECLTYPE (Xenos format in bits 0-5, signed 0x100, integer 0x200).
uint vfetchSwap(uint w, uint endian)
{
    if (endian == 1 || endian == 2)
        w = ((w & 0x00FF00FF) << 8) | ((w >> 8) & 0x00FF00FF);  // 8in16
    if (endian == 2 || endian == 3)
        w = (w << 16) | (w >> 16);  // 8in32 = 8in16 then 16in32
    return w;
}

float vfetchFraction(uint value, uint width, bool isSigned, bool isInteger, bool noZero)
{
    if (!isSigned)
        return isInteger ? float(value) : float(value) / float((1u << width) - 1);
    int s = int(value << (32 - width)) >> (32 - width);
    if (isInteger)
        return float(s);
    if (noZero)
        return (float(s) * 2.0 + 1.0) / float((1u << width) - 1);
    return max(float(s) / float((1u << (width - 1)) - 1), -1.0);
}

float4 vfetchIndirect(uint location, float index, bool rounded)
{
    uint64_t record = g_PushConstants.SharedConstants + 768 + location * 32;
    uint64_t base = vk::RawBufferLoad<uint64_t>(record, 8);
    if (base == 0)
        return float4(0.0, 0.0, 0.0, 1.0);
    uint stride = vk::RawBufferLoad<uint>(record + 8);
    int offset = vk::RawBufferLoad<int>(record + 12);
    uint type = vk::RawBufferLoad<uint>(record + 16);
    uint format = type & 0x3F;
    bool isSigned = (type & 0x100) != 0, isInteger = (type & 0x200) != 0, noZero = false;
    uint endian = uint(base & 3);
    base &= ~3ull;
    int element = int(floor(rounded ? index + 0.5 : index));
    uint64_t address = base + uint64_t(int64_t(element * int(stride) + offset) * 4);
    uint w0 = vfetchSwap(vk::RawBufferLoad<uint>(address), endian);
    float4 r = float4(0.0, 0.0, 0.0, 1.0);
    switch (format)
    {
    case 6: // k_8_8_8_8
        r = float4(vfetchFraction(w0 & 0xFF, 8, isSigned, isInteger, noZero),
                   vfetchFraction((w0 >> 8) & 0xFF, 8, isSigned, isInteger, noZero),
                   vfetchFraction((w0 >> 16) & 0xFF, 8, isSigned, isInteger, noZero),
                   vfetchFraction(w0 >> 24, 8, isSigned, isInteger, noZero));
        break;
    case 7: // k_2_10_10_10
        r = float4(vfetchFraction(w0 & 0x3FF, 10, isSigned, isInteger, noZero),
                   vfetchFraction((w0 >> 10) & 0x3FF, 10, isSigned, isInteger, noZero),
                   vfetchFraction((w0 >> 20) & 0x3FF, 10, isSigned, isInteger, noZero),
                   vfetchFraction(w0 >> 30, 2, isSigned, isInteger, noZero));
        break;
    case 25: // k_16_16
        r.xy = float2(vfetchFraction(w0 & 0xFFFF, 16, isSigned, isInteger, noZero),
                      vfetchFraction(w0 >> 16, 16, isSigned, isInteger, noZero));
        break;
    case 26: // k_16_16_16_16
    {
        uint w1 = vfetchSwap(vk::RawBufferLoad<uint>(address + 4), endian);
        r = float4(vfetchFraction(w0 & 0xFFFF, 16, isSigned, isInteger, noZero),
                   vfetchFraction(w0 >> 16, 16, isSigned, isInteger, noZero),
                   vfetchFraction(w1 & 0xFFFF, 16, isSigned, isInteger, noZero),
                   vfetchFraction(w1 >> 16, 16, isSigned, isInteger, noZero));
        break;
    }
    case 31: // k_16_16_FLOAT
        r.xy = f16tof32(uint2(w0 & 0xFFFF, w0 >> 16));
        break;
    case 32: // k_16_16_16_16_FLOAT
    {
        uint w1 = vfetchSwap(vk::RawBufferLoad<uint>(address + 4), endian);
        r = f16tof32(uint4(w0 & 0xFFFF, w0 >> 16, w1 & 0xFFFF, w1 >> 16));
        break;
    }
    case 33: // k_32
    case 34: // k_32_32
    case 35: // k_32_32_32_32
    {
        uint count = format - 32;
        r.x = isSigned ? float(int(w0)) : float(w0);
        if (count > 1) { uint w = vfetchSwap(vk::RawBufferLoad<uint>(address + 4), endian); r.y = isSigned ? float(int(w)) : float(w); }
        if (count > 2) { uint w = vfetchSwap(vk::RawBufferLoad<uint>(address + 8), endian); r.z = isSigned ? float(int(w)) : float(w); }
        if (count > 3) { uint w = vfetchSwap(vk::RawBufferLoad<uint>(address + 12), endian); r.w = isSigned ? float(int(w)) : float(w); }
        break;
    }
    case 36: // k_32_FLOAT
    case 37: // k_32_32_FLOAT
    case 38: // k_32_32_32_32_FLOAT
    case 57: // k_32_32_32_FLOAT
    {
        uint count = format == 57 ? 3 : format - 35;
        r.x = asfloat(w0);
        if (count > 1) r.y = asfloat(vfetchSwap(vk::RawBufferLoad<uint>(address + 4), endian));
        if (count > 2) r.z = asfloat(vfetchSwap(vk::RawBufferLoad<uint>(address + 8), endian));
        if (count > 3) r.w = asfloat(vfetchSwap(vk::RawBufferLoad<uint>(address + 12), endian));
        return r;  // exp_adjust does not apply to float formats
    }
    }
    return r;
}

#endif

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif

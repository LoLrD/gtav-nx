#include "shader_recompiler.h"
#include "shader_common.h"

static constexpr char SWIZZLES[] = 
{ 
    'x',
    'y', 
    'z', 
    'w', 
    '0', 
    '1',
    '_',
    '_'
};

static constexpr const char* USAGE_TYPES[] =
{
    "float4", // POSITION
    "float4", // BLENDWEIGHT
    "float4", // BLENDINDICES
    "float4", // NORMAL
    "float4", // PSIZE
    "float4", // TEXCOORD
    "float4", // TANGENT
    "float4", // BINORMAL
    "float4", // TESSFACTOR
    "float4", // POSITIONT
    "float4", // COLOR
    "float4", // FOG
    "float4", // DEPTH
    "float4", // SAMPLE
};

static constexpr const char* USAGE_VARIABLES[] =
{
    "Position",
    "BlendWeight",
    "BlendIndices",
    "Normal",
    "PointSize",
    "TexCoord",
    "Tangent",
    "Binormal",
    "TessFactor",
    "PositionT",
    "Color",
    "Fog",
    "Depth",
    "Sample"
};

static constexpr const char* USAGE_SEMANTICS[] =
{
    "POSITION",
    "BLENDWEIGHT",
    "BLENDINDICES",
    "NORMAL",
    "PSIZE",
    "TEXCOORD",
    "TANGENT",
    "BINORMAL",
    "TESSFACTOR",
    "POSITIONT",
    "COLOR",
    "FOG",
    "DEPTH",
    "SAMPLE"
};

struct DeclUsageLocation
{
    DeclUsage usage;
    uint32_t usageIndex;
    uint32_t location;
};

// GTA V: one unique Vulkan location per (usage, index). The native renderer's input layouts use
// the same table (gtav/src/native/vertex_layout.h), so keep the two in sync.
static constexpr DeclUsageLocation USAGE_LOCATIONS[] =
{
    { DeclUsage::Position, 0, 0 },
    { DeclUsage::Normal, 0, 1 },
    { DeclUsage::Tangent, 0, 2 },
    { DeclUsage::Binormal, 0, 3 },
    { DeclUsage::TexCoord, 0, 4 },
    { DeclUsage::TexCoord, 1, 5 },
    { DeclUsage::TexCoord, 2, 6 },
    { DeclUsage::TexCoord, 3, 7 },
    { DeclUsage::TexCoord, 4, 8 },
    { DeclUsage::TexCoord, 5, 9 },
    { DeclUsage::TexCoord, 6, 10 },
    { DeclUsage::TexCoord, 7, 11 },
    { DeclUsage::Color, 0, 12 },
    { DeclUsage::Color, 1, 13 },
    { DeclUsage::BlendIndices, 0, 14 },
    { DeclUsage::BlendWeight, 0, 15 },
    { DeclUsage::Position, 1, 16 },
    { DeclUsage::Position, 2, 17 },
    { DeclUsage::Position, 3, 18 },
    { DeclUsage::TexCoord, 8, 19 },
    { DeclUsage::TexCoord, 9, 20 },
    { DeclUsage::TexCoord, 10, 21 },
    { DeclUsage::TexCoord, 11, 22 },
    { DeclUsage::TexCoord, 12, 23 },
    { DeclUsage::TexCoord, 13, 24 },
    { DeclUsage::TexCoord, 14, 25 },
    { DeclUsage::TexCoord, 15, 26 },
    { DeclUsage::Normal, 1, 27 },
    { DeclUsage::Tangent, 1, 28 },
    { DeclUsage::BlendIndices, 1, 29 },
    { DeclUsage::BlendWeight, 1, 30 },
    { DeclUsage::PointSize, 0, 31 },
};

static constexpr std::pair<DeclUsage, size_t> INTERPOLATORS[] =
{
    { DeclUsage::TexCoord, 0 },
    { DeclUsage::TexCoord, 1 },
    { DeclUsage::TexCoord, 2 },
    { DeclUsage::TexCoord, 3 },
    { DeclUsage::TexCoord, 4 },
    { DeclUsage::TexCoord, 5 },
    { DeclUsage::TexCoord, 6 },
    { DeclUsage::TexCoord, 7 },
    { DeclUsage::TexCoord, 8 },
    { DeclUsage::TexCoord, 9 },
    { DeclUsage::TexCoord, 10 },
    { DeclUsage::TexCoord, 11 },
    { DeclUsage::TexCoord, 12 },
    { DeclUsage::TexCoord, 13 },
    { DeclUsage::TexCoord, 14 },
    { DeclUsage::TexCoord, 15 },
    { DeclUsage::Color, 0 },
    { DeclUsage::Color, 1 }
};

// Xenos has 32 fetch constants; each slot gets a descriptor index per dimension plus a sampler.
static constexpr uint32_t FETCH_SLOT_COUNT = 32;

static constexpr std::string_view TEXTURE_DIMENSIONS[] = 
{
    "2D",
    "3D", 
    "Cube" 
};

static FetchDestinationSwizzle getDestSwizzle(uint32_t dstSwizzle, uint32_t index)
{
    return FetchDestinationSwizzle((dstSwizzle >> (index * 3)) & 0x7);
}

void ShaderRecompiler::printDstSwizzle(uint32_t dstSwizzle, bool operand)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle >= FetchDestinationSwizzle::X && swizzle <= FetchDestinationSwizzle::W)
            out += SWIZZLES[operand ? uint32_t(swizzle) : i];
    }
}

void ShaderRecompiler::printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle == FetchDestinationSwizzle::Zero)
        {
            indent();
            println("r{}.{} = 0.0;", dstRegister, SWIZZLES[i]);
        }
        else if (swizzle == FetchDestinationSwizzle::One)
        {
            indent();
            println("r{}.{} = 1.0;", dstRegister, SWIZZLES[i]);
        }
    }
}

void ShaderRecompiler::recompile(const VertexFetchInstruction& instr, uint32_t address)
{
    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predicateCondition ? "" : "!");

        indent();
        out += "{\n";
        ++indentation;
    }

    if (!instr.isMiniFetch)
        lastFullVertexFetch = instr;
    const VertexFetchInstruction& full = instr.isMiniFetch ? lastFullVertexFetch : instr;

    // GTA V: a fetch indexed by anything but the vertex index (r0.x) reads the guest vertex
    // buffer at the computed element (vfetchIndirect, shader_common.h). The microcode leaves the
    // format and stride to D3D's bind-time patching, so the host supplies them per input
    // location from the bound vertex declaration.
    bool indirect = full.srcRegister != 0 || full.srcSwizzle != 0 || vertexIndexOverwritten;
    if (instr.dstRegister == 0 && getDestSwizzle(instr.dstSwizzle, 0) != FetchDestinationSwizzle::Keep)
        vertexIndexOverwritten = true;
    if (indirect)
    {
        auto element = vertexElements.find(address);
        uint32_t location = ~0u;
        if (element != vertexElements.end())
        {
            for (auto& usageLocation : USAGE_LOCATIONS)
            {
                if (usageLocation.usage == element->second.usage && usageLocation.usageIndex == element->second.usageIndex)
                    location = usageLocation.location;
            }
        }
        indent();
        print("r{}.", instr.dstRegister);
        printDstSwizzle(instr.dstSwizzle, false);
        if (location == ~0u)
        {
            out += " = float4(0.0, 0.0, 0.0, 1.0).";
        }
        else
        {
            print(" = vfetchIndirect({}, r{}.{}, {}).", location, full.srcRegister, SWIZZLES[full.srcSwizzle],
                  full.isIndexRounded ? "true" : "false");
        }
        printDstSwizzle(instr.dstSwizzle, true);
        out += ";\n";
        printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
        if (instr.isPredicated)
        {
            --indentation;
            indent();
            out += "}\n";
        }
        return;
    }

    indent();
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";

    auto findResult = vertexElements.find(address);
    assert(findResult != vertexElements.end());

    // GTA V: normals arrive as float inputs (the native renderer widens packed 10:10:10:2
    // data on upload), so there is no R11G11B10 unpack here.
    switch (findResult->second.usage)
    {
    case DeclUsage::TexCoord:
        print("tfetchTexcoord(g_SwappedTexcoords, ");
        break;
    }

    print("i{}{}", USAGE_VARIABLES[uint32_t(findResult->second.usage)], uint32_t(findResult->second.usageIndex));

    switch (findResult->second.usage)
    {
    case DeclUsage::TexCoord:
        print(", {})", uint32_t(findResult->second.usageIndex));
        break;
    }

    out += '.';
    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

void ShaderRecompiler::recompile(const TextureFetchInstruction& instr, bool bicubic)
{
    if (instr.opcode == FetchOpcode::SetTextureLod)
    {
        // GTA V: the LOD comes from the first selected source component.
        indent();
        println("texLod = r{}.{};", instr.srcRegister, "xyzw"[instr.srcSwizzle & 3]);
        return;
    }

    if (instr.opcode == FetchOpcode::GetTextureGradients)
    {
        // GTA V (shadow slope bias): (ddx(src.x), ddy(src.x), ddx(src.y), ddy(src.y)), coarse
        // derivatives as Xenia computes them; no texture is involved.
        if (instr.isPredicated)
        {
            indent();
            println("if ({}p0)", instr.predCondition ? "" : "!");
            indent();
            out += "{\n";
            ++indentation;
        }
        indent();
        print("r{}.", instr.dstRegister);
        printDstSwizzle(instr.dstSwizzle, false);
        if (isPixelShader)
        {
            std::string x = fmt::format("r{}.{}", instr.srcRegister, SWIZZLES[instr.srcSwizzle & 0x3]);
            std::string y = fmt::format("r{}.{}", instr.srcRegister, SWIZZLES[(instr.srcSwizzle >> 2) & 0x3]);
            print(" = float4(ddx_coarse({0}), ddy_coarse({0}), ddx_coarse({1}), ddy_coarse({1})).", x, y);
        }
        else
        {
            out += " = float4(0.0, 0.0, 0.0, 0.0).";
        }
        printDstSwizzle(instr.dstSwizzle, true);
        out += ";\n";
        printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
        if (instr.isPredicated)
        {
            --indentation;
            indent();
            out += "}\n";
        }
        return;
    }

    if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights)
        return;

    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predCondition ? "" : "!");

        indent();
        out += "{\n";
        ++indentation;
    }

    auto printSrcRegister = [&](size_t componentCount)
        {
            print("r{}.", instr.srcRegister);

            for (size_t i = 0; i < componentCount; i++)
                out += SWIZZLES[((instr.srcSwizzle >> (i * 2))) & 0x3];
        };

    std::string constName;
    const char* constNamePtr = nullptr;
#ifdef UNLEASHED_RECOMP
    bool subtractFromOne = false;
#endif

#ifdef UNLEASHED_RECOMP
    auto findResult = samplers.find(instr.constIndex);
    if (findResult != samplers.end())
    {
        constNamePtr = findResult->second;
        subtractFromOne = hasMtxPrevInvViewProjection && strcmp(constNamePtr, "sampZBuffer") == 0;
    }
    else
#endif
    {
        // Textures are bound by fetch-constant slot, not by reflection name: GTA V's shader
        // containers omit some samplers (vertex texture fetch) from the constant table.
        constName = fmt::format("s{}", instr.constIndex);
        constNamePtr = constName.c_str();
    }

#ifdef UNLEASHED_RECOMP
    if (instr.constIndex == 0 && instr.dimension == TextureDimension::Texture2D)
    {
        indent();
        print("pixelCoord = getPixelCoord({}_Texture2DDescriptorIndex, ", constNamePtr);
        printSrcRegister(2);
        out += ");\n";
    }
#endif

    // GTA V: the instruction's sampling overrides, for auditing what the HLSL does not apply yet
    // (filters: 0 point, 1 linear, 2 basemap/aniso values, 3 = use the fetch constant).
    if (instr.opcode == FetchOpcode::TextureFetch)
    {
        indent();
        println("// tfetch {} mag{} min{} mip{} aniso{} comp{} reg{} bias{}{}", constNamePtr, uint32_t(instr.magFilter),
                uint32_t(instr.minFilter), uint32_t(instr.mipFilter), uint32_t(instr.anisoFilter),
                uint32_t(instr.useCompLod), uint32_t(instr.useRegLod), instr.lodBias / 16.0f,
                instr.texCoordDenorm ? " denorm" : "");
    }

    indent();
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";
    switch (instr.opcode)
    {
    case FetchOpcode::TextureFetch:
    {
    #ifdef UNLEASHED_RECOMP
        if (subtractFromOne)
            out += "1.0 - ";
    #endif

        out += "(tfetch";  // closed with the fetch constant's exponent scale below
        break;
    }
    case FetchOpcode::GetTextureWeights:
    {
        out += "getWeights";
        break;
    }
    }

    std::string_view dimension;
    uint32_t componentCount = 0;

    switch (instr.dimension)
    {
    case TextureDimension::Texture1D:
        dimension = "1D";
        componentCount = 1;
        break;
    case TextureDimension::Texture2D:
        dimension = "2D";
        componentCount = 2;
        break;
    case TextureDimension::Texture3D:
        dimension = "3D";
        componentCount = 3;
        break;
    case TextureDimension::TextureCube:
        dimension = "Cube";
        componentCount = 3;
        break;
    }

    out += dimension;

    // GTA V: explicit LOD (register LOD, or the base level when computed LOD is off).
    bool explicitLod = instr.opcode == FetchOpcode::TextureFetch && (instr.useRegLod || !instr.useCompLod);
    if (explicitLod)
        out += "Lod";

    // GTA V: instruction-level point filter (0 = point; 3 = the fetch constant's filter).
    if (instr.opcode == FetchOpcode::TextureFetch && instr.dimension == TextureDimension::Texture2D &&
        instr.magFilter == 0 && instr.minFilter == 0)
        out += "Point";

#ifdef UNLEASHED_RECOMP
    if (bicubic && !explicitLod)
        out += "Bicubic";
#endif

    const bool sized = instr.dimension == TextureDimension::Texture1D || instr.dimension == TextureDimension::Texture2D;
    print("({0}_Texture{1}DescriptorIndex, {0}_SamplerDescriptorIndex, ", constNamePtr, dimension);
    if (sized)
        print("{}_TextureSize, ", constNamePtr);
    // GTA V: unnormalized (texel-space) coordinates are divided by the texture's size.
    bool denorm = instr.texCoordDenorm && instr.dimension != TextureDimension::TextureCube;
    if (denorm)
        print("denorm{0}({1}_Texture{0}DescriptorIndex, {2}", dimension, constNamePtr,
              sized ? fmt::format("{}_TextureSize, ", constNamePtr) : std::string());
    printSrcRegister(componentCount);
    if (denorm)
        out += ")";

    switch (instr.dimension)
    {
    case TextureDimension::Texture2D:
        print(", float2({}, {})", instr.offsetX * 0.5f, instr.offsetY * 0.5f);
        break;
    case TextureDimension::TextureCube:
        out += ", cubeMapData";
        break;
    }

    if (explicitLod)
        print(", {}{:+}", instr.useRegLod ? "texLod" : "0.0", instr.lodBias / 16.0f);

    // GTA V: the fetch constant's exp_adjust scales every fetched value by 2^exp_adjust.
    if (instr.opcode == FetchOpcode::TextureFetch)
        print(") * {}_ExpScale).", constNamePtr);
    else
        out += ").";

    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

void ShaderRecompiler::recompile(const AluInstruction& instr)
{
    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predicateCondition ? "" : "!");

        indent(); 
        out += "{\n";
        ++indentation;
    }

    enum
    {
        VECTOR_0,
        VECTOR_1,
        VECTOR_2,
        SCALAR_0,
        SCALAR_1,
        SCALAR_CONSTANT_0,
        SCALAR_CONSTANT_1
    };

    auto op = [&](size_t operand)
        {
            size_t reg = 0;
            size_t swizzle = 0;
            bool select = true;
            bool negate = false;
            bool abs = false;

            switch (operand)
            {
            case SCALAR_CONSTANT_0:
                reg = instr.src3Register;
                swizzle = instr.src3Swizzle;
                select = false;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            case SCALAR_CONSTANT_1:
                reg = (uint32_t(instr.scalarOpcode) & 1) | (instr.src3Select << 1) | (instr.src3Swizzle & 0x3C);
                swizzle = instr.src3Swizzle;
                select = true;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            default:
                switch (operand)
                {
                case VECTOR_0:
                    reg = instr.src1Register;
                    swizzle = instr.src1Swizzle;
                    select = instr.src1Select;
                    negate = instr.src1Negate;
                    break;
                case VECTOR_1:
                    reg = instr.src2Register;
                    swizzle = instr.src2Swizzle;
                    select = instr.src2Select;
                    negate = instr.src2Negate;
                    break;
                case VECTOR_2:
                case SCALAR_0:
                case SCALAR_1:
                    reg = instr.src3Register;
                    swizzle = instr.src3Swizzle;
                    select = instr.src3Select;
                    negate = instr.src3Negate;
                    break;
                }

                if (select)
                {
                    abs = (reg & 0x80) != 0;
                    reg &= 0x3F;
                }
                else
                {
                    abs = instr.absConstants;
                }

                break;
            }

            std::string regFormatted;

            if (select)
            {
                regFormatted = fmt::format("r{}", reg);
            }
            else
            {
                auto findResult = float4Constants.find(reg);
                if (findResult != float4Constants.end())
                {
                    const char* constantName = reinterpret_cast<const char*>(constantTableData + findResult->second->name);
                    if (findResult->second->registerCount > 1)
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (hasMtxProjection && strcmp(constantName, "g_MtxProjection") == 0)
                        {
                            regFormatted = fmt::format("(iterationIndex == 0 ? mtxProjectionReverseZ[{0}] : mtxProjection[{0}])",
                                reg - findResult->second->registerIndex);
                        }
                        else
                    #endif
                        {
                            regFormatted = fmt::format("{}({}{})", constantName,
                                reg - findResult->second->registerIndex, instr.const0Relative ? (instr.constAddressRegisterRelative ? " + a0" : " + aL") : "");
                        }
                    }
                    else
                    {
                        assert(!instr.const0Relative && !instr.const1Relative);
                        regFormatted = constantName;
                    }
                }
                else
                {
                    assert(!instr.const0Relative && !instr.const1Relative);
                    regFormatted = fmt::format("c{}", reg);
                }
            }

            std::string result;

            if (negate)
                result += '-';

            if (abs)
                result += "abs(";

            result += regFormatted;
            result += '.';

            switch (operand)
            {
            case VECTOR_0:
            case VECTOR_1:
            case VECTOR_2:
            {
                uint32_t mask;

                switch (instr.vectorOpcode)
                {
                case AluVectorOpcode::Dp2Add:
                    mask = (operand == VECTOR_2) ? 0b1 : 0b11;
                    break;

                case AluVectorOpcode::Dp3:
                    mask = 0b111;
                    break;

                case AluVectorOpcode::Dp4:
                case AluVectorOpcode::Max4:
                    mask = 0b1111;
                    break;

                default:
                    mask = instr.vectorWriteMask != 0 ? instr.vectorWriteMask : 0b1;
                    break;
                }

                for (size_t i = 0; i < 4; i++)
                {
                    if ((mask >> i) & 0x1)
                        result += SWIZZLES[((swizzle >> (i * 2)) + i) & 0x3];
                }

                break;
            }

            case SCALAR_0:
            case SCALAR_CONSTANT_0:
                result += SWIZZLES[((swizzle >> 6) + 3) & 0x3];
                break;

            case SCALAR_1:
            case SCALAR_CONSTANT_1:
                result += SWIZZLES[swizzle & 0x3];
                break;
            }

            if (abs)
                result += ")";

            return result;
        };

    switch (instr.vectorOpcode)
    {
    case AluVectorOpcode::KillEq:
        indent();
        println("clip(any({} == {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillGt:
        indent();
        println("clip(any({} > {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillGe:
        indent();
        println("clip(any({} >= {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillNe:
        indent();
        println("clip(any({} != {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    }

    bool closeIfBracket = false;

    std::string_view exportRegister;
    if (instr.exportData)
    {
        if (isPixelShader)
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::PSColor0:
                exportRegister = "oC0";
                break;        
            case ExportRegister::PSColor1:
                exportRegister = "oC1";
                break;        
            case ExportRegister::PSColor2:
                exportRegister = "oC2";
                break;            
            case ExportRegister::PSColor3:
                exportRegister = "oC3";
                break;           
            case ExportRegister::PSDepth:
                exportRegister = "oDepth";
                break;
            }
        }
        else
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::VSPosition:
                exportRegister = "oPos";

            #ifdef UNLEASHED_RECOMP
                if (hasMtxProjection)
                {
                    indent();
                    out += "if ((g_SpecConstants() & SPEC_CONSTANT_REVERSE_Z) == 0 || iterationIndex == 0)\n";
                    indent();
                    out += "{\n";
                    ++indentation;

                    closeIfBracket = true;
                }
            #endif

                break;

            default:
            {
                auto findResult = interpolators.find(instr.vectorDest);
                assert(findResult != interpolators.end());
                exportRegister = findResult->second;
                break;
            }
            }
        }
    }

    if (instr.vectorOpcode >= AluVectorOpcode::SetpEqPush && instr.vectorOpcode <= AluVectorOpcode::SetpGePush)
    {
        indent();
        print("p0 = {} == 0.0 && {} ", op(VECTOR_0), op(VECTOR_1));

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::SetpEqPush:
            out += "==";
            break;
        case AluVectorOpcode::SetpNePush:
            out += "!=";
            break;
        case AluVectorOpcode::SetpGtPush:
            out += ">";
            break;
        case AluVectorOpcode::SetpGePush:
            out += ">=";
            break;
        }

        out += " 0.0;\n";
    }
    else if (instr.vectorOpcode >= AluVectorOpcode::MaxA)
    {
        indent();
        println("a0 = (int)clamp(floor(({}).w + 0.5), -256.0, 255.0);", op(VECTOR_0));
    }

    uint32_t vectorWriteMask = instr.vectorWriteMask;
    if (instr.exportData)
        vectorWriteMask &= ~instr.scalarWriteMask;

    if (vectorWriteMask != 0)
    {
        if (exportRegister.empty() && (instr.vectorDest == 0 || instr.vectorDestRelative) && (vectorWriteMask & 1))
            vertexIndexOverwritten = true;
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            out += '.';
        }
        else
        {
            print("r{}.", instr.vectorDest);
        }

        for (size_t i = 0; i < 4; i++)
        {
            if ((vectorWriteMask >> i) & 0x1)
                out += SWIZZLES[i];
        }

        out += " = ";

        if (instr.vectorSaturate)
            out += "saturate(";

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::Add:
            print("{} + {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Mul:
            print("{} * {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Max:
        case AluVectorOpcode::MaxA:
        {
            const std::string a = op(VECTOR_0), b = op(VECTOR_1);
            if (a == b)
                print("maxSelf({})", a);
            else
                print("max({}, {})", a, b);
            break;
        }

        case AluVectorOpcode::Min:
        {
            const std::string a = op(VECTOR_0), b = op(VECTOR_1);
            if (a == b)
                print("minSelf({})", a);
            else
                print("min({}, {})", a, b);
            break;
        }

        case AluVectorOpcode::Seq:
            print("{} == {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sgt:
            print("{} > {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sge:
            print("{} >= {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sne:
            print("{} != {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Frc:
            print("frac({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Trunc:
            print("trunc({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Floor:
            print("floor({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Mad:
            print("{} * {} + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndEq:
            print("select({} == 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndGe:
            print("select({} >= 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndGt:
            print("select({} > 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::Dp4:
        case AluVectorOpcode::Dp3:
            print("dot({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Dp2Add:
            print("dot({}, {}) + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::Cube:
            print("cube(r{}, cubeMapData)", instr.src1Register);
            break;

        case AluVectorOpcode::Max4:
            print("max4({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::SetpEqPush:
        case AluVectorOpcode::SetpNePush:
        case AluVectorOpcode::SetpGtPush:
        case AluVectorOpcode::SetpGePush:
            print("p0 ? 0.0 : {} + 1.0", op(VECTOR_0));
            break;

        case AluVectorOpcode::KillEq:
            print("any({} == {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillGt:
            print("any({} > {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillGe:
            print("any({} >= {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillNe:
            print("any({} != {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Dst:
            print("dst({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;
        }

        if (instr.vectorSaturate)
            out += ')';

        out += ";\n";
    }

    if (instr.scalarOpcode != AluScalarOpcode::RetainPrev)
    {
        if (instr.scalarOpcode >= AluScalarOpcode::SetpEq && instr.scalarOpcode <= AluScalarOpcode::SetpRstr)
        {
            indent();
            out += "p0 = ";

            switch (instr.scalarOpcode)
            {
            case AluScalarOpcode::SetpEq:
                print("{} == 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpNe:
                print("{} != 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpGt:
                print("{} > 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpGe:
                print("{} >= 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpInv:
                print("{} == 1.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpPop:
                print("{} - 1.0 <= 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpClr:
                out += "false";
                break;

            case AluScalarOpcode::SetpRstr:
                print("{} == 0.0", op(SCALAR_0));
                break;
            }

            out += ";\n";
        }

        indent();
        out += "ps = ";
        if (instr.scalarSaturate)
            out += "saturate(";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::Adds:
            print("{} + {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::AddsPrev:
            print("{} + ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::Muls:
            print("{} * {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::MulsPrev:
        case AluScalarOpcode::MulsPrev2:
            print("{} * ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::Maxs:
        case AluScalarOpcode::MaxAs:
        case AluScalarOpcode::MaxAsf:
        {
            const std::string a = op(SCALAR_0), b = op(SCALAR_1);
            if (a == b)
                print("maxSelf({})", a);
            else
                print("max({}, {})", a, b);
            break;
        }

        case AluScalarOpcode::Mins:
        {
            const std::string a = op(SCALAR_0), b = op(SCALAR_1);
            if (a == b)
                print("minSelf({})", a);
            else
                print("min({}, {})", a, b);
            break;
        }

        case AluScalarOpcode::Seqs:
            print("{} == 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sgts:
            print("{} > 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sges:
            print("{} >= 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Snes:
            print("{} != 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Frcs:
            print("frac({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Truncs:
            print("trunc({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Floors:
            print("floor({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Exp:
            print("exp2({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Logc:
        case AluScalarOpcode::Log:
            print("clamp(log2({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Rcpc:
        case AluScalarOpcode::Rcpf:
        case AluScalarOpcode::Rcp:
            print("clamp(rcp({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Rsqc:
        case AluScalarOpcode::Rsqf:
        case AluScalarOpcode::Rsq:
            print("clamp(rsqrt({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Subs:
            print("{} - {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::SubsPrev:
            print("{} - ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpEq:
        case AluScalarOpcode::SetpNe:
        case AluScalarOpcode::SetpGt:
        case AluScalarOpcode::SetpGe:
            out += "p0 ? 0.0 : 1.0";
            break;

        case AluScalarOpcode::SetpInv:
            print("{0} == 0.0 ? 1.0 : {0}", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpPop:
            print("p0 ? 0.0 : ({} - 1.0)", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpClr:
            out += "FLT_MAX";
            break;

        case AluScalarOpcode::SetpRstr:
            print("p0 ? 0.0 : {}", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsEq:
            print("{} == 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsGt:
            print("{} > 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsGe:
            print("{} >= 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsNe:
            print("{} != 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsOne:
            print("{} == 1.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sqrt:
            print("sqrt({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Mulsc0:
        case AluScalarOpcode::Mulsc1:
            print("{} * {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Addsc0:
        case AluScalarOpcode::Addsc1:
            print("{} + {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Subsc0:
        case AluScalarOpcode::Subsc1:
            print("{} - {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Sin:
            print("sin({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Cos:
            print("cos({})", op(SCALAR_0));
            break;
        }

        if (instr.scalarSaturate)
            out += ')';

        out += ";\n";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::MaxAs:
            indent();
            println("a0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);", op(SCALAR_0));
            break;     
        case AluScalarOpcode::MaxAsf:
            indent();
            println("a0 = (int)clamp(floor({}), -256.0, 255.0);", op(SCALAR_0));
            break;
        }
    }

    uint32_t scalarWriteMask = instr.scalarWriteMask;
    if (instr.exportData)
        scalarWriteMask &= ~instr.vectorWriteMask;

    if (scalarWriteMask != 0)
    {
        if (exportRegister.empty() && (instr.scalarDest == 0 || instr.scalarDestRelative) && (scalarWriteMask & 1))
            vertexIndexOverwritten = true;
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            out += '.';
        }
        else
        {
            print("r{}.", instr.scalarDest);
        }

        for (size_t i = 0; i < 4; i++)
        {
            if ((scalarWriteMask >> i) & 0x1)
                out += SWIZZLES[i];
        }

        out += " = ps;\n";
    }

    if (instr.exportData)
    {
        uint32_t zeroMask = instr.scalarDestRelative ? (0b1111 & ~(instr.vectorWriteMask | instr.scalarWriteMask)) : 0;
        uint32_t oneMask = instr.vectorWriteMask & instr.scalarWriteMask;

        for (size_t i = 0; i < 4; i++)
        {
            uint32_t mask = 1 << i;
            if (zeroMask & mask)
            {
                indent();
                println("{}.{} = 0.0;", exportRegister, SWIZZLES[i]);
            }
            else if (oneMask & mask)
            {
                indent();
                println("{}.{} = 1.0;", exportRegister, SWIZZLES[i]);
            }
        }
    }

    if (instr.scalarOpcode >= AluScalarOpcode::KillsEq && instr.scalarOpcode <= AluScalarOpcode::KillsOne)
    {
        indent();
        out += "clip(ps != 0.0 ? -1 : 1);\n";
    }

    if (closeIfBracket)
    {
        --indentation;
        indent();
        out += "}\n";
    }

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

void ShaderRecompiler::recompile(const uint8_t* shaderData, const std::string_view& include)
{
    const auto shaderContainer = reinterpret_cast<const ShaderContainer*>(shaderData);

    assert((shaderContainer->flags & 0xFFFFFF00) == 0x102A1100);
    assert(shaderContainer->constantTableOffset != NULL);

    out += include;
    out += '\n';

    isPixelShader = (shaderContainer->flags & 0x1) == 0;
    vertexIndexOverwritten = false;

    const auto constantTableContainer = reinterpret_cast<const ConstantTableContainer*>(shaderData + shaderContainer->constantTableOffset);
    constantTableData = reinterpret_cast<const uint8_t*>(&constantTableContainer->constantTable);

    out += "#ifdef __spirv__\n\n";

#ifdef UNLEASHED_RECOMP
    bool isMetaInstancer = false;
    bool hasIndexCount = false;
#endif

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

    #ifdef UNLEASHED_RECOMP
        if (!isPixelShader)
        {
            if (strcmp(constantName, "g_MtxProjection") == 0)
                hasMtxProjection = true;
            else if (strcmp(constantName, "g_InstanceTypes") == 0)
                isMetaInstancer = true;
            else if (strcmp(constantName, "g_IndexCount") == 0)
                hasIndexCount = true;
        }
        else
        {
            if (strcmp(constantName, "g_MtxPrevInvViewProjection") == 0)
                hasMtxPrevInvViewProjection = true;
        }
    #endif

        switch (constantInfo->registerSet)
        {
        case RegisterSet::Float4:
        {
            const char* shaderName = isPixelShader ? "Pixel" : "Vertex";

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? 224 : 256) - constantInfo->registerIndex;

                println("#define {0}(INDEX) select((INDEX) < {1}, (XENOS_UBO ? g_Ubo{2}.v[{3} + min(INDEX, {4})] : vk::RawBufferLoad<float4>(g_PushConstants.{2}ShaderConstants + ({3} + min(INDEX, {4})) * 16, 0x10)), 0.0)",
                    constantName, tailCount, shaderName, constantInfo->registerIndex.get(), tailCount - 1);
            }
            else
            {
                println("#define {} (XENOS_UBO ? g_Ubo{}.v[{}] : vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + {}, 0x10))",
                    constantName, shaderName, constantInfo->registerIndex.get(), shaderName, constantInfo->registerIndex * 16);
            }
            
            for (uint16_t j = 0; j < constantInfo->registerCount; j++)
                float4Constants.emplace(constantInfo->registerIndex + j, constantInfo);

            break;
        }

        case RegisterSet::Sampler:
        {
            samplers.emplace(constantInfo->registerIndex, constantName);
            break;
        }

        }
    }

    for (uint32_t slot = 0; slot < FETCH_SLOT_COUNT; slot++)
    {
        for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
        {
            println("#define s{0}_Texture{1}DescriptorIndex (XENOS_UBO ? XENOS_SHARED_UINT({2}) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {2}))",
                slot, TEXTURE_DIMENSIONS[j], (j * FETCH_SLOT_COUNT + slot) * 4);
        }
        println("#define s{}_Texture1DDescriptorIndex s{}_Texture2DDescriptorIndex", slot, slot);
        println("#define s{0}_SamplerDescriptorIndex (XENOS_UBO ? XENOS_SHARED_UINT({1}) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {1}))",
            slot, (std::size(TEXTURE_DIMENSIONS) * FETCH_SLOT_COUNT + slot) * 4);
        println("#define s{0}_ExpScale (XENOS_UBO ? XENOS_SHARED_FLOAT({1}) : vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + {1}))",
            slot, 640 + slot * 4);
        println("#define s{0}_TextureSize (XENOS_UBO ? XENOS_SHARED_FLOAT2({1}) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + {1}))",
            slot, 1792 + slot * 8);
    }

    out += "\n#else\n\n";

    println("cbuffer {}ShaderConstants : register(b{}, space4)", isPixelShader ? "Pixel" : "Vertex", isPixelShader ? 1 : 0);
    out += "{\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Float4)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

            print("\tfloat4 {}", constantName);

            if (constantInfo->registerCount > 1)
                print("[{}]", constantInfo->registerCount.get());

            println(" : packoffset(c{});", constantInfo->registerIndex.get());

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? 224 : 256) - constantInfo->registerIndex;
                println("#define {0}(INDEX) select((INDEX) < {1}, {0}[min(INDEX, {2})], 0.0)", constantName, tailCount, tailCount - 1);
            }
        }
    }

    out += "};\n\n";

    out += "cbuffer SharedConstants : register(b2, space4)\n";
    out += "{\n";

    for (uint32_t slot = 0; slot < FETCH_SLOT_COUNT; slot++)
    {
        for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
        {
            uint32_t index = uint32_t(j) * FETCH_SLOT_COUNT + slot;
            println("\tuint s{}_Texture{}DescriptorIndex : packoffset(c{}.{});",
                slot, TEXTURE_DIMENSIONS[j], index / 4, SWIZZLES[index % 4]);
        }
        uint32_t index = uint32_t(std::size(TEXTURE_DIMENSIONS)) * FETCH_SLOT_COUNT + slot;
        println("\tuint s{}_SamplerDescriptorIndex : packoffset(c{}.{});", slot, index / 4, SWIZZLES[index % 4]);
        println("\tfloat s{}_ExpScale : packoffset(c{}.{});", slot, 40 + slot / 4, SWIZZLES[slot % 4]);
        println("\tfloat2 s{}_TextureSize : packoffset(c{}.{});", slot, 112 + slot / 2, slot % 2 ? "z" : "x");
        println("\t#define s{}_Texture1DDescriptorIndex s{}_Texture2DDescriptorIndex", slot, slot);
    }

    out += "\tDEFINE_SHARED_CONSTANTS();\n";
    out += "};\n\n";

    out += "#endif\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Bool)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);
            println("\t#define {} (1 << {})", constantName, constantInfo->registerIndex + (isPixelShader ? 16 : 0));
            boolConstants.emplace(constantInfo->registerIndex, constantName);
        }
    }

    out += '\n';

    const auto shader = reinterpret_cast<const Shader*>(shaderData + shaderContainer->shaderOffset);

    out += "#ifndef __spirv__\n";

    if (isPixelShader)
        out += "[shader(\"pixel\")]\n";
    else
        out += "[shader(\"vertex\")]\n";

    out += "#endif\n";

    out += "void main(\n";

    if (isPixelShader)
    {
        out += "\tin float4 iPos : SV_Position,\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\tin float4 i{0}{1} : {2}{1},", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);

        out += "#ifdef __spirv__\n";
        out += "\tin bool iFace : SV_IsFrontFace\n";
        out += "#else\n";
        out += "\tin uint iFace : SV_IsFrontFace\n";
        out += "#endif\n";

        auto pixelShader = reinterpret_cast<const PixelShader*>(shader);
        pixelOutputs = pixelShader->outputs;
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR0)
            out += ",\n\tout float4 oC0 : SV_Target0";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR1)
            out += ",\n\tout float4 oC1 : SV_Target1";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR2)
            out += ",\n\tout float4 oC2 : SV_Target2";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR3)
            out += ",\n\tout float4 oC3 : SV_Target3";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_DEPTH)
            out += ",\n\tout float oDepth : SV_Depth";
    }
    else
    {
        auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
        std::set<std::pair<uint32_t, uint32_t>> declaredInputs;
        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement vertexElement;
                uint32_t value;
            };

            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];

            const char* usageType = USAGE_TYPES[uint32_t(vertexElement.usage)];

            // GTA V fetches some semantics more than once (several vfetch instructions per
            // element); declare each input once, every fetch reads the same variable.
            vertexElements.emplace(uint32_t(vertexElement.address), vertexElement);
            if (!declaredInputs.emplace(uint32_t(vertexElement.usage), uint32_t(vertexElement.usageIndex)).second)
                continue;

        #ifdef UNLEASHED_RECOMP
            if ((vertexElement.usage == DeclUsage::TexCoord && vertexElement.usageIndex == 2 && isMetaInstancer) ||
                (vertexElement.usage == DeclUsage::Position && vertexElement.usageIndex == 1))
            {
                usageType = "uint4";
            }
        #endif

            out += '\t';

            for (auto& usageLocation : USAGE_LOCATIONS)
            {
                if (usageLocation.usage == vertexElement.usage && usageLocation.usageIndex == vertexElement.usageIndex)
                {
                    print("[[vk::location({})]] ", usageLocation.location);
                    break;
                }
            }

            println("in {0} i{1}{2} : {3}{2},", usageType, USAGE_VARIABLES[uint32_t(vertexElement.usage)],
                uint32_t(vertexElement.usageIndex), USAGE_SEMANTICS[uint32_t(vertexElement.usage)]);
        }

    #ifdef UNLEASHED_RECOMP
        if (hasIndexCount)
        {
            out += "\tin uint iVertexId : SV_VertexID,\n";
            out += "\tin uint iInstanceId : SV_InstanceID,\n";
        }
    #else
        // GTA V: Xenos starts every vertex shader with the vertex index (index + base) in r0.x;
        // particles build their quad corners from it.
        out += "\tin uint iVertexId : SV_VertexID,\n";
    #endif

        out += "\tout float4 oPos : SV_Position";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            print(",\n\tout float4 o{0}{1} : {2}{1}", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);
    }

    out += ")\n";
    out += "{\n";

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
    {
        specConstantsMask |= SPEC_CONSTANT_REVERSE_Z;

        out += "\toPos = 0.0;\n";

        out += "\tfloat4x4 mtxProjection = float4x4(g_MtxProjection(0), g_MtxProjection(1), g_MtxProjection(2), g_MtxProjection(3));\n";
        out += "\tfloat4x4 mtxProjectionReverseZ = mul(mtxProjection, float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1));\n";

        out += "\t[unroll] for (int iterationIndex = 0; iterationIndex < 2; iterationIndex++)\n";
        out += "\t{\n";
    }
#endif

    if (shaderContainer->definitionTableOffset != NULL)
    {
        auto definitionTable = reinterpret_cast<const DefinitionTable*>(shaderData + shaderContainer->definitionTableOffset);
        auto definitions = definitionTable->definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Float4Definition*>(definitions);
            auto value = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + definition->physicalOffset);
            for (uint16_t i = 0; i < (definition->count + 3) / 4; i++)
            {
                println("\tfloat4 c{} = asfloat(uint4(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}));",
                    definition->registerIndex + i - (isPixelShader ? 256 : 0), value[0].get(), value[1].get(), value[2].get(), value[3].get());

                value += 4;
            }
            definitions += 2;
        }
        ++definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Int4Definition*>(definitions);
            for (uint16_t i = 0; i < definition->count; i++)
            {
                union
                {
                    uint32_t value;
                    struct
                    {
                        int8_t x;
                        int8_t y;
                        int8_t z;
                        int8_t w;
                    };
                };

                value = definition->values[i].get();

                println("\tint4 i{} = int4({}, {}, {}, {});",
                    (definition->registerIndex - 8992) / 4 + i, x, y, z, w);
            }
            definitions += 2;
            definitions += definition->count;
        }

        out += "\n";
    }

    bool printedRegisters[32]{};

    uint32_t interpolatorCount = (shader->interpolatorInfo >> 5) & 0x1F;

    for (uint32_t i = 0; i < interpolatorCount; i++)
    {
        union
        {
            Interpolator interpolator;
            uint32_t value;
        };
    
        if (isPixelShader)
        {
            value = reinterpret_cast<const PixelShader*>(shader)->interpolators[i];
            println("\tfloat4 r{} = i{}{};", uint32_t(interpolator.reg), USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex));
            printedRegisters[interpolator.reg] = true;
        }
        else
        {
            auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + vertexShader->vertexElementCount + i];
            interpolators.emplace(i, fmt::format("o{}{}", USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex)));
        }
    }

    if (!isPixelShader)
    {
    #ifdef UNLEASHED_RECOMP
        if (!hasMtxProjection)
            out += "\toPos = 0.0;\n";
    #endif

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\to{}{} = 0.0;", USAGE_VARIABLES[uint32_t(usage)], usageIndex);

        out += "\n";
    }

    for (size_t i = 0; i < 32; i++)
    {
        if (!printedRegisters[i])
        {
            print("\tfloat4 r{} = ", i);
            if (isPixelShader && i == ((shader->fieldC >> 8) & 0xFF))
            {
                out += "float4((iPos.xy * g_PixelScaleInv - 0.5) * float2(iFace ? 1.0 : -1.0, 1.0), 0.0, 0.0);\n";
            }
        #ifdef UNLEASHED_RECOMP
            else if (!isPixelShader && hasIndexCount && i == 0)
            {
                out += "float4(iVertexId + g_IndexCount.x * iInstanceId, 0.0, 0.0, 0.0);\n";
            }
        #else
            else if (!isPixelShader && i == 0)
            {
                out += "float4(float(iVertexId), 0.0, 0.0, 0.0);\n";
            }
        #endif
            else
            {
                out += "0.0;\n";
            }
        }
    }

    out += "\tint a0 = 0;\n";
    out += "\tint aL = 0;\n";
    out += "\tbool p0 = false;\n";
    out += "\tfloat texLod = 0.0;\n";  // GTA V: set by setTexLOD, used by fetches with UseRegisterLOD
    out += "\tfloat ps = 0.0;\n";
    if (isPixelShader)
    {
#ifdef UNLEASHED_RECOMP
        out += "\tfloat2 pixelCoord = 0.0;\n";
#endif
        out += "\tCubeMapData cubeMapData = (CubeMapData)0;\n";
    }

    const be<uint32_t>* code = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + shader->physicalOffset);

    union
    {
        ControlFlowInstruction controlFlow[2];
        struct
        {
            uint32_t code0;
            uint32_t code1;
            uint32_t code2;
            uint32_t code3;
        };
    };

    auto controlFlowCode = code;
    uint32_t instrAddress = 0;
    uint32_t instrSize = shader->size;
    bool simpleControlFlow = true;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            uint32_t address = 0;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional || cfInstr.condJmp.direction)
                    simpleControlFlow = false;
                else
                    ++ifEndLabels[cfInstr.condJmp.address];

                break;
            }
            }

            if (address != 0)
                instrSize = std::min<uint32_t>(instrSize, address * 12);
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (simpleControlFlow)
    {
        out += '\n';
        indentation = 1;
    }
    else
    {
        out += "\n\tuint pc = 0;\n";
        out += "\twhile (true)\n";
        out += "\t{\n";
        out += "\t\tswitch (pc)\n";
        out += "\t\t{\n";
    }

    controlFlowCode = code;
    instrAddress = 0;
    uint32_t pc = 0;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            if (!simpleControlFlow)
            {
                indentation = 3;
                println("\t\tcase {}:", pc);
            }
            else
            {
                auto findResult = ifEndLabels.find(pc);
                if (findResult != ifEndLabels.end())
                {
                    for (uint32_t i = 0; i < findResult->second; i++)
                    {
                        --indentation;
                        indent();
                        out += "}\n";
                    }
                }
            }

            ++pc;

            uint32_t address = 0;
            uint32_t count = 0;
            uint32_t sequence = 0;
            bool shouldReturn = false;
            bool shouldCloseCurlyBracket = false;
            bool closeCondExec = false;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                count = cfInstr.exec.count;
                sequence = cfInstr.exec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::ExecEnd);
                break;

            // GTA V: conditional execs run their instructions only when the bool constant (or p0)
            // equals the instruction's condition; the shader end of an ...End variant applies either
            // way. Emitting them unconditionally ran GTA's wind-leaf player-collision block
            // (cexec b0) with an unset _globalEntityScale, collapsing tree leaves to a point.
            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
            {
                address = cfInstr.condExec.address;
                count = cfInstr.condExec.count;
                sequence = cfInstr.condExec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecEnd || cfInstr.opcode == ControlFlowOpcode::CondExecPredCleanEnd);

                indent();
                auto findResult = boolConstants.find(cfInstr.condExec.boolAddress);
                if (findResult != boolConstants.end())
                    println("if ((g_Booleans & {}) {}= 0)", findResult->second, cfInstr.condExec.condition ? "!" : "=");
                else if (cfInstr.condExec.boolAddress < 16)  // g_Booleans: VS bools 0-15, PS bools 0-15 << 16
                    println("if ((g_Booleans & (1u << {})) {}= 0)", uint32_t(cfInstr.condExec.boolAddress) + (isPixelShader ? 16 : 0),
                            cfInstr.condExec.condition ? "!" : "=");
                else
                    println("if ({})", cfInstr.condExec.condition ? "false" : "true");  // not in g_Booleans: reads as 0
                indent();
                out += "{\n";
                ++indentation;
                closeCondExec = true;
                break;
            }

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                count = cfInstr.condExecPred.count;
                sequence = cfInstr.condExecPred.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecPredEnd);

                indent();
                println("if ({}p0)", cfInstr.condExecPred.condition ? "" : "!");
                indent();
                out += "{\n";
                ++indentation;
                closeCondExec = true;
                break;

            case ControlFlowOpcode::LoopStart:
                if (simpleControlFlow)
                {
                    indent();
                #ifdef UNLEASHED_RECOMP
                    print("[unroll] ");
                #endif
                    println("for (aL = 0; aL < i{}.x; aL++)", uint32_t(cfInstr.loopStart.loopId));
                    indent();
                    out += "{\n";
                    ++indentation;
                }
                else 
                {
                    out += "\t\t\taL = 0;\n";
                }
                break;

            case ControlFlowOpcode::LoopEnd:
                if (simpleControlFlow)
                {
                    --indentation;
                    indent();
                    out += "}\n";
                }
                else
                {
                    out += "\t\t\t++aL;\n";
                    println("\t\t\tif (aL < i{}.x)", uint32_t(cfInstr.loopEnd.loopId));
                    out += "\t\t\t{\n";
                    println("\t\t\t\tpc = {};", uint32_t(cfInstr.loopEnd.address));
                    out += "\t\t\t\tcontinue;\n";
                    out += "\t\t\t}\n";
                }
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional)
                {
                    assert(!simpleControlFlow);
                    println("\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                    out += "\t\t\tcontinue;\n";
                }
                else
                {
                    indent();
                    if (cfInstr.condJmp.isPredicated)
                    {
                        println("if ({}p0)", cfInstr.condJmp.condition ^ simpleControlFlow ? "" : "!");
                    }
                    else
                    {
                        auto findResult = boolConstants.find(cfInstr.condJmp.boolAddress);
                        if (findResult != boolConstants.end())
                            println("if ((g_Booleans & {}) {}= 0)", findResult->second, cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
                        else
                            println("if (b{} {}= 0)", uint32_t(cfInstr.condJmp.boolAddress), cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
                    }

                    if (simpleControlFlow)
                    {
                        indent();
                        out += "{\n";
                        ++indentation;
                    }
                    else
                    {
                        out += "\t\t\t{\n";
                        println("\t\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                        out += "\t\t\t\tcontinue;\n";
                        out += "\t\t\t}\n";
                    }
                }
                break;
            }
            }

            auto instructionCode = code + address * 3;
            
            for (uint32_t i = 0; i < count; i++)
            {
                union
                {
                    VertexFetchInstruction vertexFetch;
                    TextureFetchInstruction textureFetch;
                    AluInstruction alu;
                    struct
                    {
                        uint32_t code0;
                        uint32_t code1;
                        uint32_t code2;
                    };
                };
            
                code0 = instructionCode[0];
                code1 = instructionCode[1];
                code2 = instructionCode[2];
            
                if ((sequence & 0x1) != 0)
                {
                    if (vertexFetch.opcode == FetchOpcode::VertexFetch)
                    {
                        recompile(vertexFetch, address + i);
                    }
                    else
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (textureFetch.constIndex == 10) // g_GISampler
                        {
                            specConstantsMask |= SPEC_CONSTANT_BICUBIC_GI_FILTER;

                            indent();
                            out += "if (g_SpecConstants() & SPEC_CONSTANT_BICUBIC_GI_FILTER)";
                            indent();
                            out += '{';

                            ++indentation;
                            recompile(textureFetch, true);
                            --indentation;

                            indent();
                            out += "}";
                            indent();
                            out += "else";
                            indent();
                            out += '{';

                            ++indentation;
                            recompile(textureFetch, false);
                            --indentation;

                            indent();
                            out += '}';
                        }
                        else
                    #endif
                        {
                            recompile(textureFetch, false);
                        }
                    }
                }
                else
                {
                    recompile(alu);
                }
            
                sequence >>= 2;
                instructionCode += 3;
            }

            if (closeCondExec)
            {
                --indentation;
                indent();
                out += "}\n";
            }

            if (shouldReturn)
            {
                if (isPixelShader)
                {
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TEST;

                    indent();
                    out += "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)";
                    indent();
                    out += '{';

                    indent();
                    // GTA V: RB_COLORCONTROL.alpha_func (g_AlphaFunc: 0 never, 1 less, 2 equal, 3 lequal,
                    // 4 greater, 5 notequal, 6 gequal, 7 always); faded-out LODs use "never".
                    out += "\tuint alphaFunc = g_AlphaFunc;\n";
                    indent();
                    out += "\tbool alphaPass = (alphaFunc & 1) && oC0.w < g_AlphaThreshold || (alphaFunc & 2) && oC0.w == g_AlphaThreshold || (alphaFunc & 4) && oC0.w > g_AlphaThreshold;\n";
                    indent();
                    out += "\tclip(alphaPass ? 1.0 : -1.0);\n";

                    indent();
                    out += "}";

                    // GTA V: RB_COLORCONTROL alpha-to-mask without MSAA (g_AlphaToMask = offsets
                    // | 0x100): the Xenos 2x2 dither, a pixel survives when alpha >= 1 - offset/4
                    // with its quad position's 2-bit offset, index = y low bit | x high bit (Xenia's 1-sample path).
                    if (pixelOutputs & PIXEL_SHADER_OUTPUT_COLOR0)
                    {
                        indent();
                        out += "[branch] if (g_AlphaToMask != 0)";
                        indent();
                        out += '{';
                        indent();
                        out += "\tuint2 a2mPos = uint2(iPos.xy) & 1;\n";
                        indent();
                        out += "\tuint a2mOffset = (g_AlphaToMask >> ((a2mPos.y | (a2mPos.x << 1)) * 2)) & 3;\n";
                        indent();
                        out += "\tclip(oC0.w - (1.0 - float(a2mOffset) * 0.25));\n";
                        indent();
                        out += '}';
                    }

                    for (uint32_t i = 0; i < 4; i++)
                    {
                        if (pixelOutputs & (PIXEL_SHADER_OUTPUT_COLOR0 << i))
                        {
                            indent();
                            println("oC{0} = select(xenosUnusedColor({0}), 0.0, oC{0});", i);
                        }
                    }

                    // GTA V: per-target output scale (render-target exponent bias and the 1/32
                    // of the Xenos fixed-point 16-bit formats, stored as SNORM on the host).
                    for (uint32_t i = 0; i < 4; i++)
                    {
                        if (pixelOutputs & (PIXEL_SHADER_OUTPUT_COLOR0 << i))
                        {
                            indent();
                            println("oC{0} *= g_ColorScale({0});", i);
                        }
                    }

                    if (pixelOutputs & PIXEL_SHADER_OUTPUT_COLOR0)
                    {
                        indent();
                        out += "[branch] if (g_SpecConstants() & SPEC_CONSTANT_BLEND_SKIP)\n";
                        indent();
                        out += "\tclip(oC0.w <= 0.0 ? -1 : 1);\n";
                    }

                #ifdef UNLEASHED_RECOMP
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TO_COVERAGE;

                    indent();
                    out += "else if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TO_COVERAGE)";
                    indent();
                    out += '{';

                    indent();
                    out += "\toC0.w *= 1.0 + computeMipLevel(pixelCoord) * 0.25;\n";
                    indent();
                    out += "\toC0.w = 0.5 + (oC0.w - g_AlphaThreshold) / max(fwidth(oC0.w), 1e-6);\n";

                    indent();
                    out += '}';
                #endif
                }
                else
                {
                #ifdef UNLEASHED_RECOMP
                    if (!hasMtxProjection)
                #endif
                    {
                        // GTA V: g_NdcScale/g_NdcOffset map screen-space positions (draws with
                        // the viewport transform disabled) to clip space; identity otherwise.
                        out += "\toPos.xy = oPos.xy * g_NdcScale + (g_HalfPixelOffset + g_NdcOffset) * oPos.w;\n";
                    }

                    for (size_t i = 0; i < std::size(INTERPOLATORS); i++)
                    {
                        indent();
                        println("\tif ((g_LiveOutputs & 0x{:X}u) == 0) o{}{} = 0.0;", 1u << i,
                            USAGE_VARIABLES[uint32_t(INTERPOLATORS[i].first)], INTERPOLATORS[i].second);
                    }
                }

                if (simpleControlFlow)
                {
                    indent();
                #ifdef UNLEASHED_RECOMP
                    if (hasMtxProjection)
                    {
                        out += "continue;\n";
                    }
                    else
                #endif
                    {
                        out += "return;\n";
                    }
                }
                else
                {
                    out += "\t\t\tbreak;\n";
                }
            }

            if (shouldCloseCurlyBracket)
            {
                --indentation;
                indent();
                out += "}\n";
            }
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (!simpleControlFlow)
    {
        out += "\t\t\tbreak;\n";
        out += "\t\t}\n";
        out += "\t\tbreak;\n";
        out += "\t}\n";
    }

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
        out += "\t}\n";

    if (!isPixelShader && hasMtxProjection)
        out += "\toPos.xy += g_HalfPixelOffset * oPos.w;\n";
#endif

    if (isPixelShader && simpleControlFlow)
        addKillSkip();

    out += "}";
}

void ShaderRecompiler::addKillSkip()
{
    const size_t body = out.find("void main(");
    if (body == std::string::npos)
        return;

    static constexpr std::string_view KILL_END = " ? -1 : 1);\n";
    static constexpr std::string_view PREDICATED[] = { "\n\tif (p0)\n\t{\n\t\tclip(", "\n\tif (!p0)\n\t{\n\t\tclip(" };
    static constexpr std::string_view PLAIN = "\n\tclip(";

    size_t start = std::string::npos, condition = 0;
    std::string predicate;
    for (size_t i = 0; i < std::size(PREDICATED); i++)
    {
        const size_t at = out.find(PREDICATED[i], body);
        if (at < start)
        {
            start = at;
            condition = at + PREDICATED[i].size();
            predicate = i == 0 ? "p0 && " : "!p0 && ";
        }
    }
    const size_t plain = out.find(PLAIN, body);
    if (plain < start)
    {
        start = plain;
        condition = plain + PLAIN.size();
        predicate.clear();
    }
    if (start == std::string::npos)
        return;

    const size_t conditionEnd = out.find(KILL_END, condition);
    if (conditionEnd == std::string::npos || out.find('\n', condition) < conditionEnd)
        return;

    size_t end = conditionEnd + KILL_END.size();
    if (!predicate.empty())
    {
        static constexpr std::string_view CLOSE = "\t}\n";
        if (out.compare(end, CLOSE.size(), CLOSE) != 0)
            return;
        end += CLOSE.size();
    }

    specConstantsMask |= SPEC_CONSTANT_KILL_SKIP;

    std::string live = "\n\tuint killLive = 1;\n";
    live += "\t[branch] if (g_SpecConstants() & SPEC_CONSTANT_KILL_SKIP)\n\t{\n";
    live += fmt::format("\t\tkillLive = ({}({})) ? 0u : 1u;\n", predicate, out.substr(condition, conditionEnd - condition));
    live += "\t\tkillLive |= QuadReadAcrossX(killLive);\n";
    live += "\t\tkillLive |= QuadReadAcrossY(killLive);\n";
    live += "\t}";

    out.insert(end, "\t[branch] if (killLive != 0)\n\t{\n");
    out.insert(start, live);
    out += "\t}\n";
}

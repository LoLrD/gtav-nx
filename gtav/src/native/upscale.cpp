#include "upscale.h"

#include <rex/logging.h>

#include <cmath>

namespace gtav::native {

using namespace plume;

namespace {

#include "guest_output_bilinear_ps.h"
#include "guest_output_ffx_fsr_easu_ps.h"
#include "guest_output_ffx_fsr_rcas_ps.h"
#include "guest_output_triangle_strip_rect_vs.h"

constexpr uint32_t kRectBytes = 16;
constexpr uint32_t kEffectBytes = 16;

struct EasuConstants {
  float input_output_size_ratio[2];
  float input_size_inv[2];
};

struct BilinearConstants {
  int32_t output_offset[2];
  float output_size_inv[2];
};

struct RcasConstants {
  int32_t output_offset[2];
  float sharpness_post_setup;
  float padding;
};

}  // namespace

bool SceneUpscale::Init(RenderDevice* device) {
  device_ = device;
  RenderSamplerDesc linear;
  linear.mipmapMode = RenderMipmapMode::NEAREST;
  linear.addressU = linear.addressV = linear.addressW = RenderTextureAddressMode::CLAMP;
  sampler_ = device->createSampler(linear);
  RenderDescriptorSetBuilder set;
  set.begin();
  set.addTexture(0);
  set.addImmutableSampler(1, sampler_.get());
  set.end();
  RenderPipelineLayoutBuilder layout;
  layout.begin();
  layout.addDescriptorSet(set);
  layout.addPushConstant(0, 0, kRectBytes, RenderShaderStageFlag::VERTEX, 0);
  layout.addPushConstant(1, 0, kEffectBytes, RenderShaderStageFlag::PIXEL, kRectBytes);
  layout.end();
  layout_ = layout.create(device);
  vertex_ = device->createShader(guest_output_triangle_strip_rect_vs, sizeof(guest_output_triangle_strip_rect_vs),
                                 "main", RenderShaderFormat::SPIRV);
  easu_ = device->createShader(guest_output_ffx_fsr_easu_ps, sizeof(guest_output_ffx_fsr_easu_ps), "main",
                               RenderShaderFormat::SPIRV);
  rcas_ = device->createShader(guest_output_ffx_fsr_rcas_ps, sizeof(guest_output_ffx_fsr_rcas_ps), "main",
                               RenderShaderFormat::SPIRV);
  bilinear_ = device->createShader(guest_output_bilinear_ps, sizeof(guest_output_bilinear_ps), "main",
                                   RenderShaderFormat::SPIRV);
  for (auto& slot : sets_)
    for (auto& use : slot)
      for (auto& pass : use) pass = set.create(device);
  return sampler_ && layout_ && vertex_ && easu_ && rcas_ && bilinear_;
}

void SceneUpscale::Easu(RenderCommandList* list, uint32_t slot, uint32_t use, const RenderTexture* source,
                        const RenderTextureView* view, RenderFormat format, float ratio, uint32_t source_width,
                        uint32_t source_height) {
  RenderDescriptorSet* set = sets_[slot][use][0].get();
  set->setTexture(0, source, RenderTextureLayout::SHADER_READ, view);
  const EasuConstants constants{{ratio, ratio}, {1.0f / float(source_width), 1.0f / float(source_height)}};
  Pass(list, PipelinesFor(format).easu.get(), set, &constants, sizeof(constants));
}

void SceneUpscale::Rcas(RenderCommandList* list, uint32_t slot, uint32_t use, const RenderTexture* source,
                        const RenderTextureView* view, RenderFormat format, float sharpness_stops) {
  RenderDescriptorSet* set = sets_[slot][use][1].get();
  set->setTexture(0, source, RenderTextureLayout::SHADER_READ, view);
  const RcasConstants constants{{0, 0}, std::exp2(-sharpness_stops), 0.0f};
  Pass(list, PipelinesFor(format).rcas.get(), set, &constants, sizeof(constants));
}

void SceneUpscale::Bilinear(RenderCommandList* list, uint32_t slot, uint32_t use, const RenderTexture* source,
                            const RenderTextureView* view, RenderFormat format, uint32_t width, uint32_t height) {
  RenderDescriptorSet* set = sets_[slot][use][0].get();
  set->setTexture(0, source, RenderTextureLayout::SHADER_READ, view);
  const BilinearConstants constants{{0, 0}, {1.0f / float(width), 1.0f / float(height)}};
  Pass(list, PipelinesFor(format).bilinear.get(), set, &constants, sizeof(constants));
}

SceneUpscale::Pipelines& SceneUpscale::PipelinesFor(RenderFormat format) {
  auto [it, inserted] = pipelines_.try_emplace(uint32_t(format));
  if (inserted) {
    it->second.easu = CreatePipeline(easu_.get(), format);
    it->second.rcas = CreatePipeline(rcas_.get(), format);
    it->second.bilinear = CreatePipeline(bilinear_.get(), format);
    REXLOG_INFO("[native] FSR pipelines for format {}", uint32_t(format));
  }
  return it->second;
}

std::unique_ptr<RenderPipeline> SceneUpscale::CreatePipeline(const RenderShader* pixel, RenderFormat format) {
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = vertex_.get();
  desc.pixelShader = pixel;
  desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_STRIP;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
  return device_->createGraphicsPipeline(desc);
}

void SceneUpscale::Pass(RenderCommandList* list, const RenderPipeline* pipeline, RenderDescriptorSet* set,
                        const void* constants, uint32_t size) {
  const float rect[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
  list->setGraphicsPipelineLayout(layout_.get());
  list->setPipeline(pipeline);
  list->setGraphicsDescriptorSet(set, 0);
  list->setGraphicsPushConstants(0, rect, 0, kRectBytes);
  list->setGraphicsPushConstants(1, constants, 0, size);
  list->drawInstanced(4, 1, 0, 0);
}

}  // namespace gtav::native

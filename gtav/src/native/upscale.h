#pragma once

#include <plume_render_interface.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace gtav::native {

class SceneUpscale {
 public:
  static constexpr uint32_t kSlots = 2;
  static constexpr uint32_t kUses = 2;

  bool Init(plume::RenderDevice* device);
  void Easu(plume::RenderCommandList* list, uint32_t slot, uint32_t use, const plume::RenderTexture* source,
            const plume::RenderTextureView* view, plume::RenderFormat format, float ratio, uint32_t source_width,
            uint32_t source_height);
  void Rcas(plume::RenderCommandList* list, uint32_t slot, uint32_t use, const plume::RenderTexture* source,
            const plume::RenderTextureView* view, plume::RenderFormat format, float sharpness_stops);

 private:
  struct Pipelines {
    std::unique_ptr<plume::RenderPipeline> easu, rcas;
  };

  Pipelines& PipelinesFor(plume::RenderFormat format);
  std::unique_ptr<plume::RenderPipeline> CreatePipeline(const plume::RenderShader* pixel, plume::RenderFormat format);
  void Pass(plume::RenderCommandList* list, const plume::RenderPipeline* pipeline, plume::RenderDescriptorSet* set,
            const void* constants, uint32_t size);

  plume::RenderDevice* device_ = nullptr;
  std::unique_ptr<plume::RenderSampler> sampler_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> vertex_, easu_, rcas_;
  std::unique_ptr<plume::RenderDescriptorSet> sets_[kSlots][kUses][2];
  std::unordered_map<uint32_t, Pipelines> pipelines_;
};

}  // namespace gtav::native

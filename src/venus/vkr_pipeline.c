/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_pipeline.h"

#include "vkr_pipeline_gen.h"
#include "vkr_spirv.h"

#include <stdio.h>
#include <stdlib.h>

/* Shader code given inline to a pipeline stage (maintenance5) instead of a module. */
static bool
vkr_pipeline_stages_check(const struct vkr_context *ctx,
                          const VkPipelineShaderStageCreateInfo *stages,
                          uint32_t count)
{
   for (uint32_t i = 0; i < count; i++) {
      for (const VkBaseInStructure *p = stages[i].pNext; p; p = p->pNext) {
         if (p->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
            continue;
         const VkShaderModuleCreateInfo *smci = (const VkShaderModuleCreateInfo *)p;
         if (!vkr_spirv_check(ctx->ctx_id, smci->pCode, smci->codeSize))
            return false;
      }
   }
   return true;
}

/*
 * Debugging aid: with VKR_SHADER_DUMP=<dir>, every shader module is written
 * to <dir>/<ctx>-<module>.spv and every compute pipeline is logged (to stderr)
 * before the driver sees it, so the last log line before a driver crash names
 * the shader that caused it.
 */
static once_flag vkr_shader_dump_once = ONCE_FLAG_INIT;
static const char *vkr_shader_dump_path;

static void
vkr_shader_dump_init_once(void)
{
   const char *d = getenv("VKR_SHADER_DUMP");
   vkr_shader_dump_path = d && *d ? d : NULL;
}

static const char *
vkr_shader_dump_dir(void)
{
   call_once(&vkr_shader_dump_once, vkr_shader_dump_init_once);
   return vkr_shader_dump_path;
}

static void
vkr_shader_dump_code(const struct vkr_context *ctx,
                     const char *name,
                     const uint32_t *code,
                     size_t size)
{
   char path[1024];
   FILE *f;

   snprintf(path, sizeof(path), "%s/%u-%s.spv", vkr_shader_dump_dir(), ctx->ctx_id, name);
   f = fopen(path, "wb");
   if (!f)
      return;
   fwrite(code, 1, size, f);
   fclose(f);
}

static void
vkr_shader_dump_compute(const struct vkr_context *ctx,
                        const struct vn_command_vkCreateComputePipelines *args)
{
   for (uint32_t i = 0; i < args->createInfoCount; i++) {
      const VkComputePipelineCreateInfo *ci = &args->pCreateInfos[i];
      const VkPipelineShaderStageCreateInfo *st = &ci->stage;
      /* Decoded handles still point at vkr objects; these are the driver's. */
      uint64_t module = st->module ? vn_cs_get_object_handle((const void **)&st->module,
                                                              VK_OBJECT_TYPE_SHADER_MODULE)
                                   : 0;
      uint64_t layout =
         vn_cs_get_object_handle((const void **)&ci->layout, VK_OBJECT_TYPE_PIPELINE_LAYOUT);
      uint64_t cache = args->pipelineCache
                          ? vn_cs_get_object_handle((const void **)&args->pipelineCache,
                                                    VK_OBJECT_TYPE_PIPELINE_CACHE)
                          : 0;
      char chain[256] = "";
      size_t used = 0;

      for (const VkBaseInStructure *p = st->pNext; p && used < sizeof(chain) - 16; p = p->pNext) {
         used += snprintf(chain + used, sizeof(chain) - used, " %u", p->sType);
         if (p->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            const VkShaderModuleCreateInfo *smci = (const VkShaderModuleCreateInfo *)p;
            char name[64];
            snprintf(name, sizeof(name), "inline-%" PRIu64, layout);
            vkr_shader_dump_code(ctx, name, smci->pCode, smci->codeSize);
         }
      }
      vkr_log("dump: ctx %u compute pipeline %u/%u: module %" PRIu64 " entry %s layout %" PRIu64
              " cache %" PRIu64 " flags 0x%x stage flags 0x%x spec %u stage pNext:%s",
              ctx->ctx_id, i + 1, args->createInfoCount, module,
              st->pName ? st->pName : "(null)", layout, cache, ci->flags, st->flags,
              st->pSpecializationInfo ? st->pSpecializationInfo->mapEntryCount : 0, chain);
   }
}

static void
vkr_dispatch_vkCreateShaderModule(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCreateShaderModule *args)
{
   struct vkr_context *ctx = dispatch->data;

   /* Reject invalid codeSize.
    *
    * VkShaderModuleCreateInfo is unique in the Vulkan API (as of 2023-08-22).
    * Except in rare cases, (see the `altlen` attribute in vk.xml), for each
    * typed non-void array in Vulkan, the api specifies the array length as the
    * count of array elements. But VkShaderModuleCreateInfo has a typed array
    * (uint32_t *pCode) whose length (codeSize) is specified in bytes, not as
    * a count of uint32_t.
    *
    * Also, the Vulkan 1.3.261 spec seems confused about the size of `pCode`.
    * The spec says "codeSize is the size, in bytes, of the code pointed to by
    * pCode", and then later says "pCode must be a valid pointer to an array of
    * codeSize/4 uint32_t values".
    *
    * (FWIW, VkShaderCreateInfoEXT learned from this mistake and declared the
    * array to be typeless, `void *pCode`).
    *
    * The venus encoder/decoder believes the array size is `4 * (codeSize / 4)`
    * because the vk.xml says so. For example, if codeSize is 259, then venus
    * encodes/decodes only 256 bytes. But the native driver may try to read all
    * 259 bytes, leading to out-of-bound access. Prevent the oob access by
    * validating codeSize here.
    */
   if (args->pCreateInfo->codeSize % 4 != 0) {
      vkr_context_set_fatal(ctx);
      return;
   }

   /* An invalid module is not created; a later use of it fails this context. */
   if (!vkr_spirv_check(ctx->ctx_id, args->pCreateInfo->pCode, args->pCreateInfo->codeSize)) {
      args->ret = VK_ERROR_INVALID_SHADER_NV;
      return;
   }

   struct vkr_shader_module *mod = vkr_shader_module_create_and_add(dispatch->data, args);

   if (vkr_shader_dump_dir() && mod) {
      char name[32];
      /* The driver handle: pipeline create infos carry it after decoding. */
      snprintf(name, sizeof(name), "%" PRIu64, mod->base.handle.u64);
      vkr_shader_dump_code(ctx, name, args->pCreateInfo->pCode, args->pCreateInfo->codeSize);
   }
}

static void
vkr_dispatch_vkDestroyShaderModule(struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkDestroyShaderModule *args)
{
   vkr_shader_module_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreatePipelineLayout(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCreatePipelineLayout *args)
{
   vkr_pipeline_layout_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyPipelineLayout(struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkDestroyPipelineLayout *args)
{
   vkr_pipeline_layout_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreatePipelineCache(struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCreatePipelineCache *args)
{
   vkr_pipeline_cache_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyPipelineCache(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkDestroyPipelineCache *args)
{
   vkr_pipeline_cache_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetPipelineCacheData(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkGetPipelineCacheData *args)
{
   TRACE_FUNC();
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetPipelineCacheData_args_handle(args);
   args->ret = vk->GetPipelineCacheData(args->device, args->pipelineCache,
                                        args->pDataSize, args->pData);
}

static void
vkr_dispatch_vkMergePipelineCaches(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkMergePipelineCaches *args)
{
   TRACE_FUNC();
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkMergePipelineCaches_args_handle(args);
   args->ret = vk->MergePipelineCaches(args->device, args->dstCache, args->srcCacheCount,
                                       args->pSrcCaches);
}

static void
vkr_dispatch_vkCreateGraphicsPipelines(struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCreateGraphicsPipelines *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct object_array arr;

   for (uint32_t i = 0; i < args->createInfoCount; i++) {
      const VkGraphicsPipelineCreateInfo *ci = &args->pCreateInfos[i];
      if (!vkr_pipeline_stages_check(ctx, ci->pStages, ci->stageCount)) {
         args->ret = VK_ERROR_INVALID_SHADER_NV;
         memset(args->pPipelines, 0, args->createInfoCount * sizeof(args->pPipelines[0]));
         return;
      }
   }

   if (vkr_graphics_pipeline_create_array(ctx, args, &arr) < VK_SUCCESS)
      return;

   vkr_pipeline_add_array(ctx, dev, &arr, args->pPipelines);
}

static void
vkr_dispatch_vkCreateComputePipelines(struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCreateComputePipelines *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct object_array arr;

   if (vkr_shader_dump_dir())
      vkr_shader_dump_compute(ctx, args);

   for (uint32_t i = 0; i < args->createInfoCount; i++) {
      if (!vkr_pipeline_stages_check(ctx, &args->pCreateInfos[i].stage, 1)) {
         args->ret = VK_ERROR_INVALID_SHADER_NV;
         memset(args->pPipelines, 0, args->createInfoCount * sizeof(args->pPipelines[0]));
         return;
      }
   }

   if (vkr_compute_pipeline_create_array(ctx, args, &arr) < VK_SUCCESS)
      return;

   vkr_pipeline_add_array(ctx, dev, &arr, args->pPipelines);
}

static void
vkr_dispatch_vkDestroyPipeline(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkDestroyPipeline *args)
{
   vkr_pipeline_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateRayTracingPipelinesKHR(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateRayTracingPipelinesKHR *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct object_array arr;

   for (uint32_t i = 0; i < args->createInfoCount; i++) {
      const VkRayTracingPipelineCreateInfoKHR *ci = &args->pCreateInfos[i];
      if (!vkr_pipeline_stages_check(ctx, ci->pStages, ci->stageCount)) {
         args->ret = VK_ERROR_INVALID_SHADER_NV;
         memset(args->pPipelines, 0, args->createInfoCount * sizeof(args->pPipelines[0]));
         return;
      }
   }

   if (vkr_ray_tracing_pipeline_create_array(ctx, args, &arr) < VK_SUCCESS)
      return;

   vkr_pipeline_add_array(ctx, dev, &arr, args->pPipelines);
}

static void
vkr_dispatch_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR_args_handle(args);
   args->ret = vk->GetRayTracingCaptureReplayShaderGroupHandlesKHR(
      args->device, args->pipeline, args->firstGroup, args->groupCount, args->dataSize,
      args->pData);
}

static void
vkr_dispatch_vkGetRayTracingShaderGroupHandlesKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetRayTracingShaderGroupHandlesKHR *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetRayTracingShaderGroupHandlesKHR_args_handle(args);
   args->ret = vk->GetRayTracingShaderGroupHandlesKHR(args->device, args->pipeline,
                                                      args->firstGroup, args->groupCount,
                                                      args->dataSize, args->pData);
}

static void
vkr_dispatch_vkGetRayTracingShaderGroupStackSizeKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetRayTracingShaderGroupStackSizeKHR *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetRayTracingShaderGroupStackSizeKHR_args_handle(args);
   args->ret = vk->GetRayTracingShaderGroupStackSizeKHR(args->device, args->pipeline,
                                                        args->group, args->groupShader);
}

void
vkr_context_init_shader_module_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateShaderModule = vkr_dispatch_vkCreateShaderModule;
   dispatch->dispatch_vkDestroyShaderModule = vkr_dispatch_vkDestroyShaderModule;
}

void
vkr_context_init_pipeline_layout_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreatePipelineLayout = vkr_dispatch_vkCreatePipelineLayout;
   dispatch->dispatch_vkDestroyPipelineLayout = vkr_dispatch_vkDestroyPipelineLayout;
}

void
vkr_context_init_pipeline_cache_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreatePipelineCache = vkr_dispatch_vkCreatePipelineCache;
   dispatch->dispatch_vkDestroyPipelineCache = vkr_dispatch_vkDestroyPipelineCache;
   dispatch->dispatch_vkGetPipelineCacheData = vkr_dispatch_vkGetPipelineCacheData;
   dispatch->dispatch_vkMergePipelineCaches = vkr_dispatch_vkMergePipelineCaches;
}

void
vkr_context_init_pipeline_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateGraphicsPipelines = vkr_dispatch_vkCreateGraphicsPipelines;
   dispatch->dispatch_vkCreateComputePipelines = vkr_dispatch_vkCreateComputePipelines;
   dispatch->dispatch_vkDestroyPipeline = vkr_dispatch_vkDestroyPipeline;

   /* VK_KHR_ray_tracing_pipeline */
   dispatch->dispatch_vkCreateRayTracingPipelinesKHR =
      vkr_dispatch_vkCreateRayTracingPipelinesKHR;
   dispatch->dispatch_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR =
      vkr_dispatch_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR;
   dispatch->dispatch_vkGetRayTracingShaderGroupHandlesKHR =
      vkr_dispatch_vkGetRayTracingShaderGroupHandlesKHR;
   dispatch->dispatch_vkGetRayTracingShaderGroupStackSizeKHR =
      vkr_dispatch_vkGetRayTracingShaderGroupStackSizeKHR;
}

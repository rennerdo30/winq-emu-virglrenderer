/*
 * Copyright 2026 Qorral contributors
 * SPDX-License-Identifier: MIT
 */

#include "vkr_spirv.h"

#include "vkr_common.h"

#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_VENUS_SPIRV_VALIDATE

#include <spirv-tools/libspirv.h>

enum vkr_spirv_mode {
   VKR_SPIRV_OFF,
   VKR_SPIRV_LOG,
   VKR_SPIRV_REJECT,
};

static once_flag vkr_spirv_once = ONCE_FLAG_INIT;
static enum vkr_spirv_mode vkr_spirv_mode;
static spv_context vkr_spirv_context;
static spv_validator_options vkr_spirv_options;

static void
vkr_spirv_init_once(void)
{
   const char *env = getenv("VKR_SPIRV_VALIDATE");

   if (env && (!strcmp(env, "0") || !strcmp(env, "off")))
      vkr_spirv_mode = VKR_SPIRV_OFF;
   else if (env && !strcmp(env, "log"))
      vkr_spirv_mode = VKR_SPIRV_LOG;
   else
      vkr_spirv_mode = VKR_SPIRV_REJECT;
   if (vkr_spirv_mode == VKR_SPIRV_OFF)
      return;

   /*
    * Core SPIR-V rules only (every version up to 1.6). The Vulkan environment
    * adds client-API rules (the VUID-StandaloneSpirv-* checks) that real guest
    * shaders break while drivers accept them, e.g. integer fragment inputs
    * without Flat from D3D9 translation; refusing those would break working
    * applications. Malformed modules (wrong types, bad ids) fail either way.
    */
   vkr_spirv_context = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
   vkr_spirv_options = spvValidatorOptionsCreate();
   if (!vkr_spirv_context || !vkr_spirv_options) {
      vkr_log("SPIR-V validation unavailable: SPIRV-Tools could not be initialized");
      vkr_spirv_mode = VKR_SPIRV_OFF;
      return;
   }
   /*
    * Be permissive wherever a rule depends on device features the validator
    * cannot see; the point is to catch malformed modules, not to second-guess
    * layouts the device may accept (scalar block layout and friends).
    */
   spvValidatorOptionsSetSkipBlockLayout(vkr_spirv_options, true);
   spvValidatorOptionsSetRelaxBlockLayout(vkr_spirv_options, true);
   spvValidatorOptionsSetScalarBlockLayout(vkr_spirv_options, true);
   spvValidatorOptionsSetUniformBufferStandardLayout(vkr_spirv_options, true);
   spvValidatorOptionsSetWorkgroupScalarBlockLayout(vkr_spirv_options, true);
   spvValidatorOptionsSetAllowLocalSizeId(vkr_spirv_options, true);
   spvValidatorOptionsSetAllowOffsetTextureOperand(vkr_spirv_options, true);
   spvValidatorOptionsSetFriendlyNames(vkr_spirv_options, false);
}

bool
vkr_spirv_check(uint32_t ctx_id, const uint32_t *code, size_t size)
{
   call_once(&vkr_spirv_once, vkr_spirv_init_once);
   if (vkr_spirv_mode == VKR_SPIRV_OFF)
      return true;

   /* Not const: spvValidateWithOptions takes a pointer to a mutable struct. */
   spv_const_binary_t binary = {
      .code = code,
      .wordCount = size / sizeof(uint32_t),
   };
   spv_diagnostic diag = NULL;
   const spv_result_t res =
      spvValidateWithOptions(vkr_spirv_context, vkr_spirv_options, &binary, &diag);
   if (res == SPV_SUCCESS) {
      spvDiagnosticDestroy(diag);
      return true;
   }

   const bool reject = vkr_spirv_mode == VKR_SPIRV_REJECT;
   vkr_log("ctx %u: %s invalid SPIR-V (%zu bytes, result %d): %s", ctx_id,
           reject ? "refusing" : "passing on", size, (int)res,
           diag && diag->error ? diag->error : "no details");
   spvDiagnosticDestroy(diag);
   return !reject;
}

#else /* ENABLE_VENUS_SPIRV_VALIDATE */

bool
vkr_spirv_check(UNUSED uint32_t ctx_id, UNUSED const uint32_t *code, UNUSED size_t size)
{
   return true;
}

#endif /* ENABLE_VENUS_SPIRV_VALIDATE */

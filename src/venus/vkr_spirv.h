/*
 * Copyright 2026 Qorral contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_SPIRV_H
#define VKR_SPIRV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Checks guest SPIR-V before the host driver compiles it. Host shader
 * compilers are not hardened against invalid input: an invalid module can
 * crash the driver, and with it the whole VMM, at pipeline creation. A module
 * that fails validation is refused instead, which fails only the guest
 * context that sent it.
 *
 * Returns true when the module may be passed to the driver: it is valid, or
 * validation is unavailable or switched off (VKR_SPIRV_VALIDATE=0;
 * VKR_SPIRV_VALIDATE=log validates and logs but passes everything).
 */
bool
vkr_spirv_check(uint32_t ctx_id, const uint32_t *code, size_t size);

#endif /* VKR_SPIRV_H */

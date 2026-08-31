// pm_shared/pm_math.h -- GoldSrc pm_shared integration shim for tier0 mathlib SIMD
// До: в pm_shared использовался scalar mathlib (common/mathlib.h) с ручными DotProduct/VectorNormalize
// После: подключаем tier0/mathlib.h и используем SSE-версии с детектом GetCPUInformation().m_bSSE

#pragma once
#include "../public/tier0/mathlib.h"  // тянет platform.h + GetCPUInformation + SSE интринсики

// pm_shared historically defines its own vec3_t as float[3]; tier0/mathlib.h compatible
// Экспортируемые pm_* обёртки для обратной совместимости
#ifdef __cplusplus
extern "C" {
#endif

// Используют tier0 реализации напрямую (SSE+scalar fallback)
float PM_DotProduct(const vec3_t a, const vec3_t b);
float PM_VectorNormalize(vec3_t v);
float PM_VectorLength(const vec3_t v);
void  PM_VectorAdd(const vec3_t a, const vec3_t b, vec3_t out);
void  PM_VectorScale(const vec3_t in, float scale, vec3_t out);

#ifdef __cplusplus
}
#endif

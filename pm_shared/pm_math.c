// pm_shared/pm_math.c -- пример замены скалярных вызовов на SSE-версии tier0/mathlib
// Сборка: cl /I public /I public/tier0 pm_shared/pm_math.c tier0/mathlib.cpp tier0/cpu.cpp ...

#include "pm_math.h"
#include "../public/tier0/platform.h"
#include "../public/tier0/mathlib.h"
#include <math.h>

// --- было (скаляр, common/mathlib) ---
// float DotProduct(const vec3_t a, const vec3_t b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
// float VectorNormalize(vec3_t v) {
//   float len = (float)sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
//   if (len != 0.0f) { float inv = 1.0f/len; v[0]*=inv; v[1]*=inv; v[2]*=inv; }
//   return len;
// }

// --- стало: делегируем в tier0/mathlib с runtime SSE детектом GetCPUInformation().m_bSSE ---
// tier0/mathlib.cpp уже содержит:
//   if (GetCPUInformation().m_bSSE) return DotProduct_SSE(...); else scalar
//   if (GetCPUInformation().m_bSSE) VectorNormalize via _mm_mul_ps/_mm_sqrt_ss else scalar
// pm_shared просто вызывает эти функции (или inline-версии для hot-path)

float PM_DotProduct(const vec3_t a, const vec3_t b)
{
	// hot path: можно использовать DotProduct_Inline (header-only, ветвление на m_bSSE)
	// или вызывать экспортируемую DotProduct из tier0.dll
	if (GetCPUInformation().m_bSSE)
		return DotProduct_Inline(a, b); // SSE: _mm_mul_ps + _mm_hadd_ps / shuffle fallback
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; // scalar fallback (совпадает с tier0 scalar веткой)
}

float PM_VectorNormalize(vec3_t v)
{
	// Делегируем в tier0 - там уже SSE с _mm_set_ps/_mm_mul_ps + fallback
	return VectorNormalize(v);
}

float PM_VectorLength(const vec3_t v)
{
	if (GetCPUInformation().m_bSSE)
		return VectorLength_Inline(v); // _mm_sqrt_ss после DotProduct_Inline
	return VectorLength(v);
}

void PM_VectorAdd(const vec3_t a, const vec3_t b, vec3_t out)
{
	VectorAdd_Inline(a, b, out); // внутри проверка m_bSSE, _mm_add_ps иначе scalar
}

void PM_VectorScale(const vec3_t in, float scale, vec3_t out)
{
	VectorScale_Inline(in, scale, out); // _mm_mul_ps ветка
}

// Пример использования в pm_shared физике (было: ручной scalar DotProduct в каждом трейсе):
// --- до ---
//   float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
//   float len = sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(len) { v[0]/=len; ... }
// --- после ---
//   #include "tier0/mathlib.h"
//   float d = DotProduct(a,b);           // SSE если CPU умеет, иначе scalar
//   float len = VectorNormalize(v);      // _mm_sqrt_ss + _mm_mul_ps ветка

// Примечание для сборки pm_shared (GoldSrc SDK):
// 1. Добавить в .vcxproj / Makefile: /I public/tier0 и линковку tier0.lib
// 2. В каждом .c где был #include "common/mathlib.h" заменить на #include "tier0/mathlib.h"
// 3. Все скалярные DotProduct/VectorNormalize вызовы заменить на tier0 API (совместимы по сигнатуре)

// tier0 -- mathlib SIMD (GPL-3.0)
// Reconstructed Valve-style mathlib with SSE intrinsics.
// Runtime dispatches via tier0/cpu.cpp GetCPUInformation() (m_bSSE / m_bSSE2 / m_bSSE3 / m_bAVX).
// Scalar fallback guarantees correctness on any x86.

#ifndef TIER0_MATHLIB_H
#define TIER0_MATHLIB_H
#pragma once

#include "platform.h"
#include <math.h>
#include <string.h>

#ifdef _WIN32
	#include <xmmintrin.h>   // __m128  _mm_*  SSE
	#include <emmintrin.h>   // SSE2
	#include <pmmintrin.h>   // SSE3  _mm_hadd_ps
	#include <tmmintrin.h>
	#include <intrin.h>
	#ifdef __AVX__
		#include <immintrin.h> // __m256 _mm256_* AVX (VectorNormalize batch)
	#endif
#else
	#if defined(__SSE__) || defined(__AVX__)
		#include <xmmintrin.h>
		#include <emmintrin.h>
		#include <pmmintrin.h>
		#include <immintrin.h>
	#endif
#endif

//-----------------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------------
typedef float vec_t;
typedef vec_t vec3_t[3];
typedef vec_t vec4_t[4];
typedef vec_t vec5_t[5];

// Aligned helper for SSE loads
typedef ALIGN16( vec_t ) vec3a_t[4];
typedef ALIGN16( vec_t ) vec4a_t[4];

//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD(a) ((a) * (M_PI / 180.0f))
#define RAD2DEG(a) ((a) * (180.0f / M_PI))

//-----------------------------------------------------------------------------
// Helpers: runtime CPU feature query (uses tier0/cpu.cpp)
//-----------------------------------------------------------------------------
inline bool Math_HaveSSE()  { return GetCPUInformation().m_bSSE; }
inline bool Math_HaveSSE2() { return GetCPUInformation().m_bSSE2; }
inline bool Math_HaveSSE3() { return GetCPUInformation().m_bSSE3; }
inline bool Math_HaveAVX()  { return GetCPUInformation().m_bAVX; }

//-----------------------------------------------------------------------------
// Core API  (implemented in tier0/mathlib.cpp with SSE+scalar)
//-----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

PLATFORM_INTERFACE float DotProduct(const vec3_t a, const vec3_t b);
PLATFORM_INTERFACE float DotProduct4(const vec4_t a, const vec4_t b);

PLATFORM_INTERFACE void VectorAdd(const vec3_t a, const vec3_t b, vec3_t out);
PLATFORM_INTERFACE void VectorSubtract(const vec3_t a, const vec3_t b, vec3_t out);
PLATFORM_INTERFACE void VectorCopy(const vec3_t in, vec3_t out);
PLATFORM_INTERFACE void VectorClear(vec3_t a);
PLATFORM_INTERFACE void VectorNegate(const vec3_t a, vec3_t b);
PLATFORM_INTERFACE void VectorScale(const vec3_t in, vec_t scale, vec3_t out);
PLATFORM_INTERFACE void VectorMA(const vec3_t veca, float scale, const vec3_t vecb, vec3_t vecc);
PLATFORM_INTERFACE void CrossProduct(const vec3_t a, const vec3_t b, vec3_t result);
PLATFORM_INTERFACE void Vector4Add(const vec4_t a, const vec4_t b, vec4_t out);
PLATFORM_INTERFACE void Vector4Copy(const vec4_t in, vec4_t out);
PLATFORM_INTERFACE void Vector4Scale(const vec4_t in, vec_t scale, vec4_t out);

PLATFORM_INTERFACE float VectorLength(const vec3_t v);
PLATFORM_INTERFACE float VectorLengthSqr(const vec3_t v);
PLATFORM_INTERFACE float VectorLength4(const vec4_t v);
PLATFORM_INTERFACE float VectorNormalize(vec3_t v); // in-place, returns length — SSE path, AVX _mm256_* when m_bAVX
PLATFORM_INTERFACE float VectorNormalize2(const vec3_t in, vec3_t out);
// AVX 256-bit batch: 8 vec3 in parallel via _mm256_sqrt_ps/_mm256_mul_ps, fallback to SSE
#ifdef __AVX__
PLATFORM_INTERFACE void VectorNormalize8_AVX(const vec3_t *in, vec3_t *out, float *lengths);
#endif

PLATFORM_INTERFACE int VectorCompare(const vec3_t v1, const vec3_t v2);
PLATFORM_INTERFACE void VectorLerp(const vec3_t a, const vec3_t b, float t, vec3_t out);
PLATFORM_INTERFACE float VectorDistance(const vec3_t a, const vec3_t b);
PLATFORM_INTERFACE float VectorDistanceSqr(const vec3_t a, const vec3_t b);

PLATFORM_INTERFACE float Q_rsqrt(float number); // fast inverse sqrt

#ifdef __cplusplus
}
#endif

//-----------------------------------------------------------------------------
// Inline SSE-accelerated helpers (header-only fast paths)
// These branch on GetCPUInformation().m_bSSE at runtime, otherwise scalar.
// Using __m128 as required.
//-----------------------------------------------------------------------------
inline float DotProduct_Inline(const vec3_t a, const vec3_t b)
{
	if (Math_HaveSSE())
	{
#ifdef _WIN32
		__m128 va = _mm_set_ps(0.0f, a[2], a[1], a[0]);
		__m128 vb = _mm_set_ps(0.0f, b[2], b[1], b[0]);
		__m128 mul = _mm_mul_ps(va, vb);
	#if defined(_M_IX86) || defined(_M_X64)
		if (Math_HaveSSE3())
		{
			__m128 sum = _mm_hadd_ps(mul, mul);
			sum = _mm_hadd_ps(sum, sum);
			return _mm_cvtss_f32(sum);
		}
		else
		{
			__m128 shuf = _mm_movehl_ps(mul, mul);      // [z,0,?,?]
			__m128 sums = _mm_add_ps(mul, shuf);         // [x+z, y, ?,?]
			__m128 shuf2 = _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1,1,1,1));
			__m128 dot = _mm_add_ss(sums, shuf2);
			return _mm_cvtss_f32(dot);
		}
	#else
		(void)mul;
		return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
	#endif
#else
		return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
#endif
	}
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

inline float VectorLengthSqr_Inline(const vec3_t v)
{
	return DotProduct_Inline(v, v);
}

inline float VectorLength_Inline(const vec3_t v)
{
	if (Math_HaveSSE())
	{
#ifdef _WIN32
		float sq = DotProduct_Inline(v, v);
		__m128 s = _mm_set_ss(sq);
		__m128 r = _mm_sqrt_ss(s);
		return _mm_cvtss_f32(r);
#else
		return (float)sqrt((double)(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]));
#endif
	}
	return (float)sqrt((double)(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]));
}

inline void VectorAdd_Inline(const vec3_t a, const vec3_t b, vec3_t out)
{
	if (Math_HaveSSE())
	{
#ifdef _WIN32
		__m128 va = _mm_set_ps(0.0f, a[2], a[1], a[0]);
		__m128 vb = _mm_set_ps(0.0f, b[2], b[1], b[0]);
		__m128 r = _mm_add_ps(va, vb);
		__declspec(align(16)) float tmp[4];
		_mm_store_ps(tmp, r);
		out[0]=tmp[0]; out[1]=tmp[1]; out[2]=tmp[2];
		return;
#endif
	}
	out[0]=a[0]+b[0]; out[1]=a[1]+b[1]; out[2]=a[2]+b[2];
}

inline void VectorScale_Inline(const vec3_t in, vec_t scale, vec3_t out)
{
	if (Math_HaveSSE())
	{
#ifdef _WIN32
		__m128 v = _mm_set_ps(0.0f, in[2], in[1], in[0]);
		__m128 s = _mm_set1_ps(scale);
		__m128 r = _mm_mul_ps(v, s);
		__declspec(align(16)) float tmp[4];
		_mm_store_ps(tmp, r);
		out[0]=tmp[0]; out[1]=tmp[1]; out[2]=tmp[2];
		return;
#endif
	}
	out[0]=in[0]*scale; out[1]=in[1]*scale; out[2]=in[2]*scale;
}

//-----------------------------------------------------------------------------
// Legacy macros (keep SDK compatibility)
//-----------------------------------------------------------------------------
#ifndef VectorCopy
// already function, but keep macro alias if not defined as function
#endif

#endif // TIER0_MATHLIB_H

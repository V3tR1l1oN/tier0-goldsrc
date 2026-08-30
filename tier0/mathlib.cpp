// tier0 -- mathlib SIMD (GPL-3.0)
// Implements public/tier0/mathlib.h with SSE intrinsics + scalar fallback.
// Runtime dispatch via GetCPUInformation() from tier0/cpu.cpp.

#include "platform.h"
#include "../public/tier0/mathlib.h"

#include <math.h>

#ifdef _WIN32
	#include <xmmintrin.h>
	#include <emmintrin.h>
	#include <pmmintrin.h>
#endif

// SSE helpers ---------------------------------------------------------------
static inline float DotProduct_SSE(const vec3_t a, const vec3_t b)
{
#ifdef _WIN32
	__m128 va = _mm_set_ps(0.0f, a[2], a[1], a[0]);
	__m128 vb = _mm_set_ps(0.0f, b[2], b[1], b[0]);
	__m128 mul = _mm_mul_ps(va, vb);
	if (GetCPUInformation().m_bSSE3)
	{
		__m128 sum = _mm_hadd_ps(mul, mul);
		sum = _mm_hadd_ps(sum, sum);
		return _mm_cvtss_f32(sum);
	}
	else
	{
		__m128 shuf = _mm_movehl_ps(mul, mul);
		__m128 sums = _mm_add_ps(mul, shuf);
		__m128 shuf2 = _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1,1,1,1));
		__m128 dot = _mm_add_ss(sums, shuf2);
		return _mm_cvtss_f32(dot);
	}
#else
	return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
#endif
}

static inline float VectorLengthSqr_SSE(const vec3_t v)
{
	return DotProduct_SSE(v, v);
}

static inline float VectorLength_SSE(const vec3_t v)
{
#ifdef _WIN32
	float sq = DotProduct_SSE(v, v);
	__m128 s = _mm_set_ss(sq);
	__m128 r = _mm_sqrt_ss(s);
	return _mm_cvtss_f32(r);
#else
	return (float)sqrt((double)(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]));
#endif
}

//-----------------------------------------------------------------------------
// Exported API (PLATFORM_INTERFACE == extern "C")
//-----------------------------------------------------------------------------

extern "C" float DotProduct(const vec3_t a, const vec3_t b)
{
	if (GetCPUInformation().m_bSSE)
		return DotProduct_SSE(a, b);
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

extern "C" float DotProduct4(const vec4_t a, const vec4_t b)
{
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 va = _mm_loadu_ps(a);
		__m128 vb = _mm_loadu_ps(b);
		__m128 mul = _mm_mul_ps(va, vb);
		if (GetCPUInformation().m_bSSE3)
		{
			__m128 sum = _mm_hadd_ps(mul, mul);
			sum = _mm_hadd_ps(sum, sum);
			return _mm_cvtss_f32(sum);
		}
		else
		{
			// SSE2 hadd emulation: add hl
			__m128 shuf = _mm_movehl_ps(mul, mul);
			__m128 sums = _mm_add_ps(mul, shuf);
			__m128 shuf2 = _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1,1,1,0));
			// sums = [x+z, y+w, ?, ?] -> need sums0+sums1
			__m128 t = _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1,1,1,1));
			__m128 dot = _mm_add_ss(sums, t);
			return _mm_cvtss_f32(dot);
		}
#endif
	}
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
}

extern "C" void VectorAdd(const vec3_t a, const vec3_t b, vec3_t out)
{
	if (GetCPUInformation().m_bSSE)
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

extern "C" void VectorSubtract(const vec3_t a, const vec3_t b, vec3_t out)
{
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 va = _mm_set_ps(0.0f, a[2], a[1], a[0]);
		__m128 vb = _mm_set_ps(0.0f, b[2], b[1], b[0]);
		__m128 r = _mm_sub_ps(va, vb);
		__declspec(align(16)) float tmp[4];
		_mm_store_ps(tmp, r);
		out[0]=tmp[0]; out[1]=tmp[1]; out[2]=tmp[2];
		return;
#endif
	}
	out[0]=a[0]-b[0]; out[1]=a[1]-b[1]; out[2]=a[2]-b[2];
}

extern "C" void VectorCopy(const vec3_t in, vec3_t out)
{
	// tiny, use scalar or SSE move
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 v = _mm_set_ps(0.0f, in[2], in[1], in[0]);
		__declspec(align(16)) float tmp[4];
		_mm_store_ps(tmp, v);
		out[0]=tmp[0]; out[1]=tmp[1]; out[2]=tmp[2];
		return;
#endif
	}
	out[0]=in[0]; out[1]=in[1]; out[2]=in[2];
}

extern "C" void VectorClear(vec3_t a)
{
#ifdef _WIN32
	if (GetCPUInformation().m_bSSE)
	{
		__m128 z = _mm_setzero_ps();
		__declspec(align(16)) float tmp[4];
		_mm_store_ps(tmp, z);
		a[0]=tmp[0]; a[1]=tmp[1]; a[2]=tmp[2];
		return;
	}
#endif
	a[0]=a[1]=a[2]=0.0f;
}

extern "C" void VectorNegate(const vec3_t a, vec3_t b)
{
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 va = _mm_set_ps(0.0f, a[2], a[1], a[0]);
		__m128 z = _mm_setzero_ps();
		__m128 r = _mm_sub_ps(z, va);
		__declspec(align(16)) float tmp[4];
		_mm_store_ps(tmp, r);
		b[0]=tmp[0]; b[1]=tmp[1]; b[2]=tmp[2];
		return;
#endif
	}
	b[0]=-a[0]; b[1]=-a[1]; b[2]=-a[2];
}

extern "C" void VectorScale(const vec3_t in, vec_t scale, vec3_t out)
{
	if (GetCPUInformation().m_bSSE)
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

extern "C" void VectorMA(const vec3_t veca, float scale, const vec3_t vecb, vec3_t vecc)
{
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 va = _mm_set_ps(0.0f, veca[2], veca[1], veca[0]);
		__m128 vb = _mm_set_ps(0.0f, vecb[2], vecb[1], vecb[0]);
		__m128 s = _mm_set1_ps(scale);
		__m128 mul = _mm_mul_ps(vb, s);
		__m128 r = _mm_add_ps(va, mul);
		__declspec(align(16)) float tmp[4];
		_mm_store_ps(tmp, r);
		vecc[0]=tmp[0]; vecc[1]=tmp[1]; vecc[2]=tmp[2];
		return;
#endif
	}
	vecc[0]=veca[0]+scale*vecb[0];
	vecc[1]=veca[1]+scale*vecb[1];
	vecc[2]=veca[2]+scale*vecb[2];
}

extern "C" void CrossProduct(const vec3_t a, const vec3_t b, vec3_t result)
{
	// SSE shuffle version when available, fallback scalar
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 va = _mm_set_ps(0.0f, a[2], a[1], a[0]);
		__m128 vb = _mm_set_ps(0.0f, b[2], b[1], b[0]);
		// shuffle to compute cross = (a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x)
		__m128 va_yzx = _mm_shuffle_ps(va, va, _MM_SHUFFLE(3,0,2,1)); // [0, x, z, y]
		__m128 vb_zxy = _mm_shuffle_ps(vb, vb, _MM_SHUFFLE(3,1,0,2)); // [0, y, x, z]
		__m128 va_zxy = _mm_shuffle_ps(va, va, _MM_SHUFFLE(3,1,0,2));
		__m128 vb_yzx = _mm_shuffle_ps(vb, vb, _MM_SHUFFLE(3,0,2,1));
		__m128 c1 = _mm_mul_ps(va_yzx, vb_zxy);
		__m128 c2 = _mm_mul_ps(va_zxy, vb_yzx);
		__m128 cross = _mm_sub_ps(c1, c2);
		__declspec(align(16)) float tmp[4];
		_mm_store_ps(tmp, cross);
		result[0]=tmp[0]; result[1]=tmp[1]; result[2]=tmp[2];
		return;
#endif
	}
	result[0]=a[1]*b[2]-a[2]*b[1];
	result[1]=a[2]*b[0]-a[0]*b[2];
	result[2]=a[0]*b[1]-a[1]*b[0];
}

extern "C" void Vector4Add(const vec4_t a, const vec4_t b, vec4_t out)
{
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 va = _mm_loadu_ps(a);
		__m128 vb = _mm_loadu_ps(b);
		__m128 r = _mm_add_ps(va, vb);
		_mm_storeu_ps(out, r);
		return;
#endif
	}
	out[0]=a[0]+b[0]; out[1]=a[1]+b[1]; out[2]=a[2]+b[2]; out[3]=a[3]+b[3];
}

extern "C" void Vector4Copy(const vec4_t in, vec4_t out)
{
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 v = _mm_loadu_ps(in);
		_mm_storeu_ps(out, v);
		return;
#endif
	}
	out[0]=in[0]; out[1]=in[1]; out[2]=in[2]; out[3]=in[3];
}

extern "C" void Vector4Scale(const vec4_t in, vec_t scale, vec4_t out)
{
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 v = _mm_loadu_ps(in);
		__m128 s = _mm_set1_ps(scale);
		__m128 r = _mm_mul_ps(v, s);
		_mm_storeu_ps(out, r);
		return;
#endif
	}
	out[0]=in[0]*scale; out[1]=in[1]*scale; out[2]=in[2]*scale; out[3]=in[3]*scale;
}

extern "C" float VectorLength(const vec3_t v)
{
	if (GetCPUInformation().m_bSSE)
		return VectorLength_SSE(v);
	return (float)sqrt((double)(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]));
}

extern "C" float VectorLengthSqr(const vec3_t v)
{
	if (GetCPUInformation().m_bSSE)
		return VectorLengthSqr_SSE(v);
	return v[0]*v[0]+v[1]*v[1]+v[2]*v[2];
}

extern "C" float VectorLength4(const vec4_t v)
{
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 va = _mm_loadu_ps(v);
		__m128 mul = _mm_mul_ps(va, va);
		float sq;
		if (GetCPUInformation().m_bSSE3)
		{
			__m128 sum = _mm_hadd_ps(mul, mul);
			sum = _mm_hadd_ps(sum, sum);
			sq = _mm_cvtss_f32(sum);
		}
		else
		{
			__m128 shuf = _mm_movehl_ps(mul, mul);
			__m128 sums = _mm_add_ps(mul, shuf);
			__m128 shuf2 = _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1,1,1,0));
			__m128 t = _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1,1,1,1));
			__m128 dot = _mm_add_ss(sums, t);
			sq = _mm_cvtss_f32(dot);
		}
		__m128 s = _mm_set_ss(sq);
		__m128 r = _mm_sqrt_ss(s);
		return _mm_cvtss_f32(r);
#endif
	}
	return (float)sqrt((double)(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]+v[3]*v[3]));
}

extern "C" float VectorNormalize(vec3_t v)
{
	float len = VectorLength(v);
	if (len != 0.0f)
	{
		float inv = 1.0f / len;
		if (GetCPUInformation().m_bSSE)
		{
#ifdef _WIN32
			__m128 vec = _mm_set_ps(0.0f, v[2], v[1], v[0]);
			__m128 s = _mm_set1_ps(inv);
			__m128 r = _mm_mul_ps(vec, s);
			__declspec(align(16)) float tmp[4];
			_mm_store_ps(tmp, r);
			v[0]=tmp[0]; v[1]=tmp[1]; v[2]=tmp[2];
			return len;
#endif
		}
		v[0]*=inv; v[1]*=inv; v[2]*=inv;
	}
	return len;
}

extern "C" float VectorNormalize2(const vec3_t in, vec3_t out)
{
	float len = VectorLength(in);
	if (len != 0.0f)
	{
		float inv = 1.0f/len;
		if (GetCPUInformation().m_bSSE)
		{
#ifdef _WIN32
			__m128 vec = _mm_set_ps(0.0f, in[2], in[1], in[0]);
			__m128 s = _mm_set1_ps(inv);
			__m128 r = _mm_mul_ps(vec, s);
			__declspec(align(16)) float tmp[4];
			_mm_store_ps(tmp, r);
			out[0]=tmp[0]; out[1]=tmp[1]; out[2]=tmp[2];
			return len;
#endif
		}
		out[0]=in[0]*inv; out[1]=in[1]*inv; out[2]=in[2]*inv;
	}
	else
	{
		out[0]=out[1]=out[2]=0.0f;
	}
	return len;
}

extern "C" int VectorCompare(const vec3_t v1, const vec3_t v2)
{
	// exact float compare as SDK
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 a = _mm_set_ps(0.0f, v1[2], v1[1], v1[0]);
		__m128 b = _mm_set_ps(0.0f, v2[2], v2[1], v2[0]);
		__m128 cmp = _mm_cmpeq_ps(a, b);
		int mask = _mm_movemask_ps(cmp);
		// low 3 bits must be 1 (x,y,z equal), high bit is 0==0 -> 1 as well, so mask == 0xF for equality
		// Check only low 3
		return (mask & 0x7) == 0x7;
#endif
	}
	return (v1[0]==v2[0] && v1[1]==v2[1] && v1[2]==v2[2]);
}

extern "C" void VectorLerp(const vec3_t a, const vec3_t b, float t, vec3_t out)
{
	// out = a + t*(b-a)
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 va = _mm_set_ps(0.0f, a[2], a[1], a[0]);
		__m128 vb = _mm_set_ps(0.0f, b[2], b[1], b[0]);
		__m128 diff = _mm_sub_ps(vb, va);
		__m128 s = _mm_set1_ps(t);
		__m128 mul = _mm_mul_ps(diff, s);
		__m128 r = _mm_add_ps(va, mul);
		__declspec(align(16)) float tmp[4];
		_mm_store_ps(tmp, r);
		out[0]=tmp[0]; out[1]=tmp[1]; out[2]=tmp[2];
		return;
#endif
	}
	out[0]=a[0]+t*(b[0]-a[0]);
	out[1]=a[1]+t*(b[1]-a[1]);
	out[2]=a[2]+t*(b[2]-a[2]);
}

extern "C" float VectorDistance(const vec3_t a, const vec3_t b)
{
	vec3_t d;
	VectorSubtract(a,b,d);
	return VectorLength(d);
}

extern "C" float VectorDistanceSqr(const vec3_t a, const vec3_t b)
{
	vec3_t d;
	VectorSubtract(a,b,d);
	return VectorLengthSqr(d);
}

extern "C" float Q_rsqrt(float number)
{
	// SSE rsqrt with one Newton iteration when SSE available
	if (GetCPUInformation().m_bSSE)
	{
#ifdef _WIN32
		__m128 x = _mm_set_ss(number);
		__m128 r = _mm_rsqrt_ss(x);
		// one iteration: r = r * (1.5 - 0.5*number*r*r)
		__m128 half = _mm_set_ss(0.5f);
		__m128 threeHalves = _mm_set_ss(1.5f);
		__m128 nr = _mm_mul_ss(r, r);
		nr = _mm_mul_ss(nr, x);
		nr = _mm_mul_ss(nr, half);
		nr = _mm_sub_ss(threeHalves, nr);
		r = _mm_mul_ss(r, nr);
		return _mm_cvtss_f32(r);
#endif
	}
	// scalar fallback
	float x2 = number * 0.5f;
	float y = number;
	long i = *(long*)&y;
	i = 0x5f3759df - (i >> 1);
	y = *(float*)&i;
	y = y * (1.5f - (x2 * y * y));
	return y;
}

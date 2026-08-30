// test_mathlib -- validates tier0/mathlib SIMD vs scalar fallback
// Compile with: cl /I public/tier0 /I public /I tier0 test_mathlib.cpp tier0/cpu.cpp tier0/mathlib.cpp tier0/platform.cpp /Fetest_mathlib.exe
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../public/tier0/platform.h"
#include "../public/tier0/mathlib.h"

static int g_fails = 0;
#define CHECK(cond,msg) do{ if(!(cond)){ printf("FAIL: %s\n",msg); g_fails++; } else { /*printf(" ok: %s\n",msg);*/ } }while(0)
#define EPS 1e-4f
static bool feq(float a,float b){ return fabsf(a-b) < EPS; }

static float DotScalar(const vec3_t a,const vec3_t b){ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }

int main()
{
	printf("mathlib test -- CPU SSE=%d SSE2=%d SSE3=%d AVX=%d\n",
		(int)GetCPUInformation().m_bSSE,
		(int)GetCPUInformation().m_bSSE2,
		(int)GetCPUInformation().m_bSSE3,
		(int)GetCPUInformation().m_bAVX);

	// 1. DotProduct
	{
		vec3_t a={1,2,3}, b={4,5,6};
		float d = DotProduct(a,b);
		CHECK(feq(d,32.0f),"DotProduct 1*4+2*5+3*6==32");
		CHECK(feq(d,DotScalar(a,b)),"DotProduct matches scalar");
		// via inline
		CHECK(feq(DotProduct_Inline(a,b),32.0f),"DotProduct_Inline");
	}
	{
		vec3_t a={0,0,0}, b={1,2,3};
		CHECK(feq(DotProduct(a,b),0.0f),"DotProduct zero");
	}

	// 2. VectorLength / LengthSqr
	{
		vec3_t v={3,4,0};
		CHECK(feq(VectorLength(v),5.0f),"VectorLength 3,4,0 ==5");
		CHECK(feq(VectorLengthSqr(v),25.0f),"VectorLengthSqr");
		CHECK(feq(VectorLength_Inline(v),5.0f),"VectorLength_Inline");
		vec3_t z={0,0,0};
		CHECK(feq(VectorLength(z),0.0f),"VectorLength zero");
	}

	// 3. VectorNormalize
	{
		vec3_t v={3,0,0};
		float len=VectorNormalize(v);
		CHECK(feq(len,3.0f),"VectorNormalize len");
		CHECK(feq(v[0],1.0f)&&feq(v[1],0.0f)&&feq(v[2],0.0f),"VectorNormalize result");

		vec3_t in={0,0,5}, out;
		float len2=VectorNormalize2(in,out);
		CHECK(feq(len2,5.0f),"VectorNormalize2 len");
		CHECK(feq(out[2],1.0f),"VectorNormalize2 out");

		vec3_t zero={0,0,0};
		float lz=VectorNormalize(zero);
		CHECK(feq(lz,0.0f)&&feq(zero[0],0.0f),"VectorNormalize zero");
	}

	// 4. VectorAdd / Subtract / Scale / MA
	{
		vec3_t a={1,2,3}, b={4,5,6}, out;
		VectorAdd(a,b,out);
		CHECK(feq(out[0],5)&&feq(out[1],7)&&feq(out[2],9),"VectorAdd");
		VectorSubtract(b,a,out);
		CHECK(feq(out[0],3)&&feq(out[1],3)&&feq(out[2],3),"VectorSubtract");
		VectorScale(a,2.0f,out);
		CHECK(feq(out[0],2)&&feq(out[1],4)&&feq(out[2],6),"VectorScale");
		VectorMA(a,2.0f,b,out); // a+2*b = [9,12,15]
		CHECK(feq(out[0],9)&&feq(out[1],12)&&feq(out[2],15),"VectorMA");
		// inline
		VectorAdd_Inline(a,b,out);
		CHECK(feq(out[0],5),"VectorAdd_Inline");
		VectorScale_Inline(a,2.0f,out);
		CHECK(feq(out[0],2),"VectorScale_Inline");
	}

	// 5. CrossProduct
	{
		vec3_t a={1,0,0}, b={0,1,0}, out;
		CrossProduct(a,b,out);
		CHECK(feq(out[0],0)&&feq(out[1],0)&&feq(out[2],1),"CrossProduct x cross y = z");
		vec3_t c={0,0,1}, d={0,1,0};
		CrossProduct(c,d,out);
		CHECK(feq(out[0],-1)&&feq(out[1],0)&&feq(out[2],0),"CrossProduct z cross y = -x");
	}

	// 6. VectorClear / Copy / Negate / Compare
	{
		vec3_t v={1,2,3}, out;
		VectorCopy(v,out);
		CHECK(VectorCompare(v,out),"VectorCopy+Compare");
		VectorClear(out);
		CHECK(feq(out[0],0)&&feq(out[1],0)&&feq(out[2],0),"VectorClear");
		VectorNegate(v,out);
		CHECK(feq(out[0],-1)&&feq(out[1],-2)&&feq(out[2],-3),"VectorNegate");
		vec3_t w={1,2,3};
		CHECK(VectorCompare(v,w),"VectorCompare true");
		vec3_t u={1,2,4};
		CHECK(!VectorCompare(v,u),"VectorCompare false");
	}

	// 7. Vector4
	{
		vec4_t a={1,2,3,4}, b={5,6,7,8}, out;
		CHECK(feq(DotProduct4(a,b),70.0f),"DotProduct4");
		Vector4Add(a,b,out);
		CHECK(feq(out[0],6)&&feq(out[3],12),"Vector4Add");
		Vector4Scale(a,2.0f,out);
		CHECK(feq(out[0],2)&&feq(out[3],8),"Vector4Scale");
		CHECK(feq(VectorLength4(a),(float)sqrt(30.0)),"VectorLength4");
	}

	// 8. Lerp / Distance
	{
		vec3_t a={0,0,0}, b={10,10,10}, out;
		VectorLerp(a,b,0.5f,out);
		CHECK(feq(out[0],5)&&feq(out[1],5)&&feq(out[2],5),"VectorLerp");
		CHECK(feq(VectorDistance(a,b),(float)sqrt(300.0)),"VectorDistance");
		CHECK(feq(VectorDistanceSqr(a,b),300.0f),"VectorDistanceSqr");
	}

	// 9. Q_rsqrt
	{
		float r = Q_rsqrt(4.0f);
		CHECK(feq(r,0.5f),"Q_rsqrt 4 ==0.5");
		CHECK(feq(1.0f/r,2.0f),"Q_rsqrt inverse");
	}

	// 10. stress: compare SSE vs scalar on random vectors
	{
		srand(12345);
		for(int i=0;i<10000;i++){
			vec3_t av={(float)rand()/RAND_MAX*10-5,(float)rand()/RAND_MAX*10-5,(float)rand()/RAND_MAX*10-5};
			vec3_t bv={(float)rand()/RAND_MAX*10-5,(float)rand()/RAND_MAX*10-5,(float)rand()/RAND_MAX*10-5};
			float s = DotScalar(av,bv);
			float v = DotProduct(av,bv);
			if(!feq(s,v)){
				printf(" mismatch dot %f vs %f\n",s,v);
				g_fails++;
				break;
			}
		}
		CHECK(g_fails==0,"stress DotProduct random 10k");
	}

	printf(g_fails?"--- %d FAILURES ---\n":"--- all ok ---\n", g_fails);
	return g_fails?1:0;
}

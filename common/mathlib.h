// common/mathlib.h -- совместимый шим для GoldSrc SDK (common/mathlib -> tier0/mathlib SIMD)
// Исторически GoldSrc хранит mathlib в common/mathlib.h с скалярными функциями.
// Этот шим перенаправляет на tier0/mathlib.h (SSE + GetCPUInformation().m_bSSE детект).

#pragma once

// Подключаем актуальную реализацию
#include "../public/tier0/mathlib.h"

// Алиасы для старого SDK кода (если где-то использовался #include "mathlib.h" без tier0/):
// Ничего дополнительно не требуется - vec_t/vec3_t/DotProduct/VectorNormalize совпадают.
// Для сборки common/mathlib обычно компилирует common/mathlib.c со скалярными версиями.
// Рекомендация: в common/mathlib.c заменить:
//   #include "mathlib.h"  ->  #include "tier0/mathlib.h"
//   scalar DotProduct     ->  вызов tier0 DotProduct (или DotProduct_Inline)
// См. pm_shared/pm_math.c для примера замены.


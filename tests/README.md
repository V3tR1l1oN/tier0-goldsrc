# tests — функциональные проверки `tier0.dll`

Набор изолированных тестов, которые грузят собранную `tier0.dll` в процесс и
проверяют поведение профилировщика (`CVProfile`) напрямую, без игры.

| Файл | Что проверяет |
|---|---|
| `test_vprof.cpp` | Основной регрессионный тест: `Start` → `EnterScope/ExitScope` (дерево `Root_Frame → Update/Physics + Render`), повторное использование узлов между кадрами, `GetNumBudgetGroups`, `MarkFrame`, `Stop`. Чистый выход (раньше на teardown падал: double-free в `CVProfile::Term` — исправлено) |
| `test_mem.cpp` | Корректность аллокатора по оригиналу: `Alloc(0)` не-NULL и пишется, округление малых размеров, `Realloc(NULL,n)`==`Alloc(n)`, `Realloc(p,0)` **не free** (блок остаётся живым), префикс при сжатии, `Free(NULL)`, `GetVersion()==0`, `IsDebugHeap()==false`; плюс покрытие SBA: `GetSize` == точному округлению (весь 1..96 + сэмпл 97..2048, `Alloc(0)==4`), кросс-бакетовые `Realloc` 96↔97 (GetSize 96↔104) и 2048↔2049 (переход SBA↔CRT) с сохранением префикса, чурн 4096 блоков × 50 раундов |
| `sba_diag.cpp` | Диагностика SBA под нагрузкой: `sba_diag.exe iters size size2` — фазы alloc/check/free, проверка round/GetSize каждого блока, верфи анализ (`live GetSize`), общая маркировка `--- sba_diag done ---`. Перед `DumpStats` ставит `SpewOutputFunc`(→stderr): честно у оригинала дефолтный выводчик возвращает `SPEW_DEBUGGER`, и `Msg()` через `__debugbreak()` — в голом харнессе без установленного выводчика это брейкпоинт, как и сделало бы в игре до установки своего выводчика движком |
| `test_sba_stress.cpp` | Многопоточный стресс аллокатора: N потоков чурнят смешанные размеры (4..65536, включая CRT-крупные), мгновенная проверка содержимого под контенцией; затем фаза cross-thread handoff — рабочие потоки аллоцируют, главный проверяет `GetSize`+содержимое и освобождает (паттерн «много производителей → главный free»); `test_sba_stress.exe 16 600` ≈ 5 млн alloc/free + handoff, ожидается `--- sba stress ok ---` |
| `test_crash.cpp` | Проверка краш-лога: `test_crash.exe heap` → должна писаться запись `Reason=HEAP_CORRUPTION`, `test_crash.exe brk` → `Reason=BREAKPOINT`; процесс падает с соответствующим EXCEPTION-кодом |
| `test_getsize2048.cpp` | Регрессия живых крупных блоков: 2000 блоков по 2048 Б с мгновенной проверкой `GetSize==2048` и `Realloc 64->2048` с сохранением префикса и `GetSize==2048`. Плюс CRT-крупные: точный `GetSize` на 20480 и 65536, `Realloc 256->20480/65536`, переезд `20480/65536 <-> 2048` в обе стороны. Валидирует адаптивные слабы 8192 Б, таблицу `bucket -> payload` и честный размер крупных блоков |
| `probe_malloc.cpp` | Контроль скорости «голому» CRT-аллокатору (80k×4096 alloc+write+free ≈ 92–97 ms) — бенчмарк, доказывающий, что узкое место было не в CRT-куче |
| `test_exports.cpp` | Сверка экспортного манифеста: 313/313 имён из `tier0.def`, ординалы строго 1..313, каждый экспорт резолвится `GetProcAddress` и по имени, и по ординалу в один и тот же адрес; вызов безопасного подмножества (`Plat_FloatTime`, `Plat_MSTime`) |
| `bench.cpp` | Бенчмарк внутренних хот-путей: стоимость `Plat_FloatTime`/`Plat_MSTime` (ns/call), стабильность часов (гэпы между соседними отсчётами frametime в чистом прогоне и под шумом-аллокатором), pacing тиков (`ThreadSleep(10/7)` — реальные интервалы пробуждения, чистые и под боевой загрузкой; legacy `Sleep` даёт 15.0/14.6 мс, точный сон — 10.02/7.00 мс даже под аллокаторным шумом), трип `Alloc`+`Free` по размером 4..65536, `Realloc` 64→2048, `GetSize` живых крупных блоков 8192/65536, thread create+join, кросс-поточный churn (4 потока alloc → main free); проверка `GetSize` (реальный размер ≥ заказанного) |
| `test_sba_mt.cpp` | Харнесс контенции глобального лока SBA: 1/4/16 потоков × чурн 4..2048 (`test_sba_mt.exe iters`); выводит ms и ns/op на поток — помогает принимать решения о переносе локов |
| `diag2.cpp` | Пошаговый полный поток `EnterScope` с логом в `trace*.log` (использовался при разработке) |
| `diag3.cpp` | SEH-инструментированный прогон тех же вызовов (использовался для отладки краша построения дерева) |

## Сборка и запуск

Нужен **x86-промпт** MSVC (как для основной сборки) — `cl` для 32-битной цели.

```
cl /O1 /GS- /nologo tests\test_vprof.cpp /Fetests\test_vprof.exe /link /SUBSYSTEM:CONSOLE user32.lib
cl /O1 /GS- /nologo tests\test_mem.cpp /Fetests\test_mem.exe /link /SUBSYSTEM:CONSOLE
cl /O1 /GS- /nologo tests\test_exports.cpp /Fetests\test_exports.exe /link /SUBSYSTEM:CONSOLE
cl /O1 /GS- /nologo tests\sba_diag.cpp /Fetests\sba_diag.exe /link /SUBSYSTEM:CONSOLE
cl /O2 /GS- /nologo tests\test_sba_stress.cpp /Fetests\test_sba_stress.exe /link /SUBSYSTEM:CONSOLE
cl /O1 /GS- /nologo tests\test_crash.cpp /Fetests\test_crash.exe /link /SUBSYSTEM:CONSOLE
cl /O1 /GS- /nologo tests\test_getsize2048.cpp /Fetests\test_getsize2048.exe /link /SUBSYSTEM:CONSOLE
cl /O2 /GS- /nologo tests\test_sba_mt.cpp /Fetests\test_sba_mt.exe /link /SUBSYSTEM:CONSOLE
```

Для удобства есть скрипты `build_bench.bat`, `build_sba_diag.bat`,
`build_sba_stress.bat` (сами зовут `vcvars32.bat`).

Тест грузит `tier0.dll` через `LoadLibrary`, поэтому запускать его нужно,
**положив `build\tier0.dll` рядом с exe** (или в текущую директорию):

```
copy build\tier0.dll tests\
tests\test_vprof.exe
```

Ожидаемый результат (`test_vprof.exe`):

```
--- node tree ---
"Root_Frame" (budget=0 calls=1)
  "Update" (budget=1 calls=2)     <- calls=2: узел переиспользован во 2-м кадре
  "Physics" (budget=1 calls=1)
  "Render" (budget=2 calls=1)
budget groups registered: 3
--- all ok ---
exit code 0
```

Любое отклонение (краш, кривое дерево, не-3 группы, `calls≠2` у Update на
втором кадре) — признак регрессии в `tier0/vprof.cpp`.

`test_exports.exe` запускается из корня репо (ему нужен `tier0.def` рядом —
сверка эталонных имён/ординалов), с `tests\tier0.dll` на месте:

```
tests\test_exports.exe
export manifest: 313 total (1 DATA), ordinals 1..313
--- all ok ---
```

Вся строка `export manifest: 313 total ...` плюс `--- all ok ---` — признак того,
что ни один экспорт не потерялся, имя каждого экспорта резолвится в тот же
адрес, что и его ординал, и безопасное подмножество функций реально вызывается.

## Ожидаемые результаты SBA-тестов

```
tests\sba_diag.exe 200000 4 2048
---
refill 200000 (t=491ms)          <- тысячи аллокаций 4->2048 за полсекунды
--- sba_diag done (t=600ms) ---
exit code 0

tests\test_sba_stress.exe 16 600
--- sba stress ok (16 threads x 600 rounds x 512 blocks) ---
exit code 0

tests\test_getsize2048.exe
Alloc(2048) x2000: 2000/2000 GetSize==2048
Realloc(64->2048): GetSize=2048 OK
--- ok ---
exit code 0

tests\test_sba_mt.exe 6000000
contended alloc/free  size 4..2048  per-thread iters=375000
   1 threads:     ~12 ms  ( ~32 ns/op)
   4 threads:    ~18 ms  ( ~12 ns/op)
  16 threads:    ~56 ms  (  ~9 ns/op)
ok
exit code 0
```

Любой не-нулевой exit, краш, `live GetSize`≠`0xFFFFFFFF` или расхождение
содержимого в стресс-тесте — признак регрессии в `tier0/mem.cpp`
(ресайклинг/хэш-карта слабов/таблицы хот-пути/адаптивные слабы).

`test_sba_mt.exe 6000000` — главный воспроизводитель lock-free гонки
`SBASlabMap` (редкий ACCESS_VIOLATION при арене + фоновом прекеше):
после фикса прогон 15× подряд — чистый exit 0. Переменные окружения для
арены: `SBA_ARENA=0` (fallback на CRT malloc), `SBA_ARENA_MB` (target
прекеша, default = min(резерв, max(64, freeRAM/8))), `SBA_RESERVE_MB`
(дефолт 512), `SBA_PREPAGE=0` (отключить фоновый поток).

`test_crash.exe` пишет в `crash.log` рядом с DLL:
`Reason=HEAP_CORRUPTION` / `Reason=BREAKPOINT` с дампом регистров и стека.
Запускать по одному процессу на режим (процесс завершается по exeption-коду).

## Сборка diag2/diag3 / bench

```
cl /O1 /GS- /nologo tests\diag2.cpp /Fetests\diag2.exe /link /SUBSYSTEM:CONSOLE user32.lib
cl /O1 /GS- /nologo tests\diag3.cpp /Fetests\diag3.exe /link /SUBSYSTEM:CONSOLE user32.lib
cl /O1 /GS- /nologo tests\bench.cpp /Fetests\bench.exe /link /SUBSYSTEM:CONSOLE
```

Запуск `diag2`/`diag3`/`bench` так же с `tier0.dll` рядом; `diag2`/`diag3`
пишут отладочные строки в `trace.log`/`trace2.log`/`trace3.log` рядом с exe.
`bench` выводит числа в консоль — сравнивайте их между сборками, чтобы
объективно видеть, стало ли быстрее/стабильнее после правок
(`Plat_FloatTime` должен быть монотонным, `GetSize >= ExpectRound(size)` во всех
строках, вся строка `GetSize ... OK` и `Realloc ... OK` — иначе регрессия).

Тесты используют голые `__thiscall`-обёртки (`__declspec(naked)`): они читают
аргументы со стекa и зовут целевой метод, не полагаясь на заголовки. Ничего не
линкуется напрямую с DLL — все экспорты берутся через `GetProcAddress`. Это
позволяет проверять нашу сборку без внутренних зависимостей на header/order.
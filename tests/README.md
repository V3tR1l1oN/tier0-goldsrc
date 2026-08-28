# tests — функциональные проверки `tier0.dll`

Набор изолированных тестов, которые грузят собранную `tier0.dll` в процесс и
проверяют поведение профилировщика (`CVProfile`) напрямую, без игры.

| Файл | Что проверяет |
|---|---|
| `test_vprof.cpp` | Основной регрессионный тест: `Start` → `EnterScope/ExitScope` (дерево `Root_Frame → Update/Physics + Render`), повторное использование узлов между кадрами, `GetNumBudgetGroups`, `MarkFrame`, `Stop`. Чистый выход (раньше на teardown падал: double-free в `CVProfile::Term` — исправлено) |
| `test_mem.cpp` | Корректность аллокатора по оригиналу: `Alloc(0)` не-NULL и пишется, округление малых размеров, `Realloc(NULL,n)`==`Alloc(n)`, `Realloc(p,0)` **не free** (блок остаётся живым), префикс при сжатии, `Free(NULL)`, `GetVersion()==0`, `IsDebugHeap()==false` |
| `test_exports.cpp` | Сверка экспортного манифеста: 313/313 имён из `tier0.def`, ординалы строго 1..313, каждый экспорт резолвится `GetProcAddress` и по имени, и по ординалу в один и тот же адрес; вызов безопасного подмножества (`Plat_FloatTime`, `Plat_MSTime`) |
| `bench.cpp` | Бенчмарк внутренних хот-путей: стоимость `Plat_FloatTime` (ns/call), трип `Alloc`+`Free` по размером 4..65536, `Realloc` 64→2048; проверка `GetSize` (реальный размер ≥ заказанного) |
| `diag2.cpp` | Пошаговый полный поток `EnterScope` с логом в `trace*.log` (использовался при разработке) |
| `diag3.cpp` | SEH-инструментированный прогон тех же вызовов (использовался для отладки краша построения дерева) |

## Сборка и запуск

Нужен **x86-промпт** MSVC (как для основной сборки) — `cl` для 32-битной цели.

```
cl /O1 /GS- /nologo tests\test_vprof.cpp /Fetests\test_vprof.exe /link /SUBSYSTEM:CONSOLE user32.lib
cl /O1 /GS- /nologo tests\test_mem.cpp /Fetests\test_mem.exe /link /SUBSYSTEM:CONSOLE
cl /O1 /GS- /nologo tests\test_exports.cpp /Fetests\test_exports.exe /link /SUBSYSTEM:CONSOLE
```

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
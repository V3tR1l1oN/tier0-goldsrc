# tests — функциональные проверки `tier0.dll`

Набор изолированных тестов, которые грузят собранную `tier0.dll` в процесс и
проверяют поведение профилировщика (`CVProfile`) напрямую, без игры.

| Файл | Что проверяет |
|---|---|
| `test_vprof.cpp` | Основной регрессионный тест: `Start` → `EnterScope/ExitScope` (дерево `Root_Frame → Update/Physics + Render`), повторное использование узлов между кадрами, `GetNumBudgetGroups`, `MarkFrame`, `Stop` |
| `diag2.cpp` | Пошаговый полный поток `EnterScope` с логом в `trace*.log` (использовался при разработке) |
| `diag3.cpp` | SEH-инструментированный прогон тех же вызовов (использовался для отладки краша построения дерева) |

## Сборка и запуск

Нужен **x86-промпт** MSVC (как для основной сборки) — `cl` для 32-битной цели.

```
cl /O1 /GS- /nologo tests\test_vprof.cpp /Fetests\test_vprof.exe /link /SUBSYSTEM:CONSOLE user32.lib
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

## Сборка diag2/diag3

```
cl /O1 /GS- /nologo tests\diag2.cpp /Fetests\diag2.exe /link /SUBSYSTEM:CONSOLE user32.lib
cl /O1 /GS- /nologo tests\diag3.cpp /Fetests\diag3.exe /link /SUBSYSTEM:CONSOLE user32.lib
```

Запуск так же с `tier0.dll` рядом; `diag2`/`diag3` пишут отладочные строки в
`trace.log`/`trace2.log`/`trace3.log` рядом с exe.

Тесты используют голые `__thiscall`-обёртки (`__declspec(naked)`): они читают
аргументы со стекa и зовут целевой метод, не полагаясь на заголовки. Ничего не
линкуется напрямую с DLL — все экспорты берутся через `GetProcAddress`. Это
позволяет проверять нашу сборку без внутренних зависимостей на header/order.
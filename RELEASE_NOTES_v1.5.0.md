# v1.5.0 — synchronization, startup safety, and crash-handler hardening

This release keeps the GoldSrc ABI contract unchanged: the DLL remains an x86 replacement with the same 313 exports and ordinals 1–313. It focuses on correctness under multithreaded startup, worker-thread calls, allocator fallback, and crash reporting.

## What changed

- **Synchronized `CWorkerThread` call slot**
  - Atomically reserves the one-slot request queue before publishing its payload.
  - Keeps `WT_PENDING` set while the worker processes the request.
  - Clears `WT_PENDING | WT_SCHEDULED` only after `Reply` publishes the result.
  - Resets the completion event before a new request, preventing a stale asynchronous signal from completing a later synchronous call early.
- **Corrected thread primitive behavior**
  - Parameterless `CThreadFastMutex::Lock()` now waits with `TT_INFINITE` instead of acting as an immediate timeout.
  - `WaitForMultipleEvents` rejects empty lists, null list pointers, and null handle entries before calling Win32.
- **Thread-safe first-use initialization**
  - QPC timer state, CPU vendor information, and `CPUInformation` initialization now use atomic state transitions.
  - `Plat_FloatTime` falls back to `GetTickCount` if QPC initialization is unavailable.
  - CPU probing checks the maximum supported basic and extended CPUID leaves before querying them.
- **Affinity-aware CPU topology fallback**
  - Physical and logical processor counts continue to honor the process affinity mask even when the modern topology API or its allocation fails.
- **Safer crash logging**
  - The vectored crash handler prevents recursive entry while the logger is reading memory or writing files.
  - EBP-chain walking and stack scanning validate committed, readable memory and region bounds before dereferencing stack addresses.
- **New and expanded regression coverage**
  - Added `tests/test_threadtools.cpp` for the exported worker-thread ABI, async-to-sync calls, repeated replies, termination, and handle validation.
  - Added `tests/test_regress.cpp` for VProf re-entry, unmatched `Stop`, sibling-subtree cleanup, and `CValidator` deep-copy behavior.
  - CI now runs the new tests, exercises allocator fallback with `SBA_ARENA=0`, and includes the threadtools ABI test in the normal suite.

## Verification

The verified x86 build and test run completed with:

```text
BUILD=OK
TEST_BUILD=OK
test_threadtools: OK
--- all ok --- (0 failures)
```

The verification also covered 313 exports, allocator and cross-thread stress, VProf, CPU detection with default and restricted affinity, fallback allocator mode, benchmark smoke, and the intentional crash-log/mini-dump smoke test. The crash test is expected to terminate with the heap-corruption exception because it deliberately triggers that condition.

## Compatibility and installation

- Intended for 32-bit GoldSrc/Steam game processes, including Counter-Strike 1.6, Half-Life, Day of Defeat, Team Fortress Classic, and compatible mods.
- The public ABI and export ordinals are unchanged.
- Back up the original `tier0.dll` before installing the release next to `hl.exe`.
- The release DLL is built for x86 with MSVC and Windows SDK tooling.

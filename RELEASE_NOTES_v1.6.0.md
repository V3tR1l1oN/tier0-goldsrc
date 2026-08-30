# v1.6.0 — allocator shutdown hardening and fully green CI

This release keeps the GoldSrc ABI contract unchanged: the DLL remains an x86 replacement with the same 313 exports and ordinals 1–313. It focuses on tearing down the slab allocator safely, making worker-thread calls fail closed instead of hanging, and getting every CI check to pass.

## What changed

- **SBA arena shutdown hardened** (`tier0/mem.cpp`)
  - The pre-page helper now wakes on a dedicated `hWake` event instead of sleeping blindly, and the arena lifecycle is a 4-state machine (0 uninit / 1 init / 2 ready / 3 disabled).
  - `SBArena_Stop()` joins the helper thread *before* slab backing is released, closing the use-after-free window.
  - Teardown deliberately leaks the process-lifetime reservation rather than risk freeing live blocks if the helper cannot join.
- **Thread lifecycle fail-closed** (`tier0/threadtools.cpp`)
  - `CThread::Join` fails closed on `WAIT_FAILED`, captures the thread exit code into `m_result`, and closes the handle.
  - `Stop()` treats a negative timeout as infinite.
  - `Terminate()` refuses to self-terminate the caller, keeps the handle owned until `Join`/dtor (real synchronization), and waits for the kernel object to signal.
  - `CWorkerThread::Call` now fails closed on reply timeout by terminating the worker and poisoning the one-slot queue. Timeout is configurable via `TIER0_WORKER_TIMEOUT_MS` (default 30 s).
- **New / expanded regression coverage**
  - Added `tests/test_sba_shutdown.cpp` exercising the pre-page + arena lifecycle and clean shutdown.
  - `test_threadtools` adds a stuck-worker timeout regression using a full 12-slot `CWorkerThread` vtable (slot 10 = WaitForReply) so the reply-wait dispatch no longer reads out of bounds.
  - CI runs both tests in the main and fallback (`SBA_ARENA=0`) suites.
- **CI fixes**
  - Export-count verification now matches the real dumpbin output (`313 number of functions`), confirming all 313 exports.
  - Crash-log smoke uses BREAKPOINT mode (artificial `0xC0000374` can take the runtime fail-fast path and bypass tier0's vectored handler) and resets `$LASTEXITCODE` after the intentional crash so the pwsh step is not falsely flagged.
  - All 12 CI steps are green: build, export check, full and fallback test suites, MT stress, CPU report, crash-log smoke, and benchmark smoke.

## Verification

All 12 CI steps pass on `main` (`87282a1`):

```text
Build tier0.dll                      OK
Verify export count (313)            OK
Build test harnesses                 OK
Run test suite                       OK
Run MT stress (long)                 OK
CPU report contract                  OK
Run fallback mode suite (SBA_ARENA=0) OK
Crash-log smoke                      OK
Benchmark smoke                      OK
```

## Compatibility and installation

- Intended for 32-bit GoldSrc/Steam game processes, including Counter-Strike 1.6, Half-Life, Day of Defeat, Team Fortress Classic, and compatible mods.
- The public ABI and export ordinals are unchanged.
- Back up the original `tier0.dll` before installing the release next to `hl.exe`.
- The release DLL (`tier0.dll`, attached to this release) is built for x86 with MSVC and Windows SDK tooling.

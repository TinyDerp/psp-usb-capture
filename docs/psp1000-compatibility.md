# PSP-1000 / 6.60 PRO-C2 capture compatibility

## Report and evidence

A PSP-1004 running 6.60 PRO-C2 answered the v10 USB handshake and snapshot
commands, but the unmodified native viewer received zero video frames while
the game was visibly moving. Repeated snapshots produced a usable picture.
This points to the frame notification path rather than a missing USB driver.
It does not identify the firmware-specific cause conclusively.

The original audio capture was not verified on that console. This branch
addresses startup and missing-export cases that can prevent audio hooks
from observing output; successful audio on the PSP-1004 still needs a test.

## Changes

- Reapply the syscall hooks after module-start callbacks, chaining the previous
  handler first, and again when starting a stream. PRO patches existing syscall
  entries; resolving an export before the game's imports are linked does not
  guarantee that subsequent game calls will reach the hook.
- If no real display callback arrives for 250 ms, sample the framebuffer getter
  at up to 15 Hz. A bounded receive timeout services this path even with zero
  hook notifications. A real callback takes priority, and frame IDs remain
  monotonic when hooks resume. Static screens also get an initial picture.
- Keep each available audio hook when another optional export is absent, and
  retry missing exports as modules load. Keep channel reservations across
  retries. Unknown or oversized PCM blocks are dropped before reading memory.
- Report polling via the additive `PSPCAP_STATS_POLLING` bit. Protocol v10 packet
  layouts are unchanged, so the existing Windows viewer remains compatible.

In polling mode, the viewer's "game fps" counter reflects capture samples,
not the game's actual rendering rate. Capture still takes PSP CPU time and
USB bandwidth. Audio requires working hooks and valid channel reservations;
the framebuffer fallback cannot recover sound by itself.

## Validation

The host tests compile the actual display, audio, and hook-lifecycle sources
against mocked PSP APIs. They check absent notifications, pacing, recovery to
real callbacks, a callback arriving during vblank, disabled capture, invalid
framebuffer queries, partial audio exports, hook retries, PCM bounds, ring
wraparound, and imports linked after the plugin starts. The existing pixel
format tests run alongside them.

On Windows with Visual Studio's C/C++ tools, without vcpkg or the PSP SDK:

```powershell
cmake -S tests -B out/host-tests -A Win32
cmake --build out/host-tests --config Release
ctest --test-dir out/host-tests -C Release --output-on-failure
```

The test target needs 32-bit pointers to model PSP function addresses. The
actual plugin builds with the official PSP SDK using the existing workflow:

```sh
make -C psp GIT_REV="$(git rev-parse --short HEAD)"
```

Host tests and a cross-build do not establish PSP hardware stability. A test
of the new plugin on the affected console is still required.

## Later hardware test

These steps replace the running plugin and interrupt capture. Schedule them
after finishing any session that uses the Python snapshot viewer.

1. Close all PC capture viewers and return the PSP to USB file-transfer mode.
2. Back up the current `seplugins/pspcap.prx` and `game.txt`.
3. Replace only `pspcap.prx` with the test build; keep one enabled entry in
   `game.txt`. Restart the PSP and the game so reservation hooks run from startup.
4. The USB ID remains `054C:01CF`; the existing WinUSB driver can stay installed.
5. Start the existing Windows viewer with `--565 --yield 200`. Test video first,
   including loading screens and level changes, for at least ten minutes.
6. Restart the viewer with `--565 --yield 200 --audio`. Check game sound, timing,
   and stability. Save the PC output and `seplugins/pspcap.log` if it fails.

To roll back, close the viewer, restore the backed-up plugin through USB
file-transfer mode, and restart the PSP. No firmware flashing is involved.

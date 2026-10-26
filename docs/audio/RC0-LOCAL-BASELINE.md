# LIMINAL Audio RC0 — isolated local installation receipt

Prepared on the owner's Windows 11 device, 2026-10-26. **Not a public game release.**

- Origin: `play-pripyat-audio-v1.2` (installed IX-Ray Acoustic v2, old v3 rooms).
- Destination: `D:\Development\LIMINAL-IX\play-liminal-audio-rc0`.
- Copied: **16,890 files**, **22,061,228,520 bytes**.
- Original source/profile untouched; `fsgame.ltx` rebased to destination. External Living Zone archives are intentionally shared from the owner's existing installation.
- Reproducer: `python tools/release/stage_local_audio.py --source <source> --target <fresh_empty_path> --apply`. Never run on an existing game folder.
- SHA-256 of staged core binary/data:
  - `bin/xrEngine.exe`: `33f221ca495316257efdbdd9b73ca34c9c406306fe33aa1034509cf41f692128`
  - `bin/xrGame.dll`: `643d18fed80f006925d0cfe4901f77725bd179a4e8bcc75386edea7e1cff1799`
  - `bin/xrSound.dll`: `8f7bee9f2f1a937bd9329dd6390db17de685b6b1d6903b319d95af1fd037ba29`
  - `bin/SDL3.dll`: `582be0d13dc2f2d276fbb7b91c223376a0db1a803ab7b34b7aa74ff6769808c5`
  - `level.snd_env`: `b53fd25a3da5265ef2fe938ba957c1d0e5aa568e42cb310fbff5e1cda0318087`
  - `level.snd_rooms`: `6db2a481798522ed1bd528fb5f3d0d58ef50dc6c21403c9038c7ca22a11314fa`
  - Destination `fsgame.ltx`: `b861a8762d6e3b1a9c37a97d7bca8dff1389ac953458aea16f3f793e019e5200`.

## Minimal runtime observation

The cloned `xrEngine.exe` launched with `-fsltx fsgame.ltx -r4 -autotest`, mounted `LIMINAL Audio Configuration 20261026` and `LIMINAL Acoustic Tract v2`, initialized WASAPI 44100 Hz stereo F32 and 48 Resonance HRTFs, and created an engine log. The attempted 12-second gameplay auto-scenario **did not start the world** and timed out after 90 seconds with 0/120 frames. Only menu music was confirmed playing. This is **NOT a passed in-game smoke**, and cannot demonstrate native acoustic behavior in the new folder. It does not prove the executable crashed or lost its assets; the earlier installed source profile ran an actual game scene.

Known inherited startup warnings include missing `item_document_1.ogf` and `kanistra_01.ogf` visuals; these are not fixed in the audio-only branch.

The **complete physical v6 atlas remains blocked**; do not substitute `UNINSTALLABLE-geometry-audit.snd_rooms`. At the next audio iteration, fix the minimal game-start fixture/command and run one short actual in-world event. No movement or camera changes.

## GitHub rights boundary

No third-party game binaries, IX-Ray New Sound Engine source, GAMMA/STCoP/Paradox banks, assets or original map binaries are committed to the public `liminal/audio` branch. The game owner's local installed originals are only referenced by stage tooling.

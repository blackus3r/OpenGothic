# L'Hiver Uriziel runtime regression tests

These opt-in integration tests load a real save, enable Marvin god mode, insert the
actual L'Hiver NPC instances, exercise their Daedalus callbacks, and write
machine-checkable JSON results.

They intentionally do not claim equivalence with the original Gothic engine.
They verify OpenGothic behavior against the installed L'Hiver Uriziel data and
make the two reported regressions reproducible.

## Tests

- `enemy-heal-combat`: inserts `WOLF`, `BLOODFLY`, and `GOBBO_GREEN`. Each NPC
  enters the real `ZS_MM_ATTACK` state and runs L'Hiver's
  `B_MM_ASSESSDAMAGE` callback eight times at melee distance. Any HP increase,
  front-cone failure, or obstructed control LOS fails the test. A parallel
  negative control evaluates the former head-bone/model-yaw calculation; the
  run also fails if that legacy calculation never reproduces the old FOV bug.
- `orc-behind-detection`: places an Orc Warrior 271 cm in front of the player
  while facing away. It proves separately that the directional FOV is false and
  records which installed callbacks the regular perception flow can invoke.
  Stock senses independently report `SENSE_SMELL` behind the Orc. In the tested
  L'Hiver state, the installed `B_MM_ASSESSENEMY` callback instead reaches
  `updateNearestEnemy()`, which acquires the player through its `freeLos=true`
  selector. The second case disables smell and hearing to isolate sight: the
  directional sense then stays false while that regular free-LOS path still
  succeeds. FightAI/turning and behavior observed before the later visual
  fight-state fallback are reported independently. If L'Hiver's callback does
  not enter FightAI in that run, the recording uses a deterministic
  visualization fallback labelled `FightAI=FIXTURE`; that fallback is not
  treated as evidence of detection.

Before inserting NPCs, the harness scans terrain for a flat, unobstructed 3D
volume with no unrelated NPC inside OpenGothic's 30-metre active-NPC radius.
This prevents fences or save-specific geometry from turning an environmental
LOS block into a false perception regression, and prevents farmers or animals
from influencing the Orc AI. Among valid volumes it chooses the one with the
largest measured distance to the nearest NPC. Because the L'Hiver save can
contain roaming monsters just outside that radius, the harness temporarily
moves NPCs from a 60-metre buffer to 90 metres, records every move, and restores
their positions and headings before exit. It reapplies this isolation before
every perception sample, because L'Hiver's far AI can teleport actors back to
their routine positions. The Orc test then rechecks the active radius, records
the maximum count, and requires it to remain zero throughout both cases.

## macOS runner

Build and run against L'Hiver Uriziel save slot 5:

```sh
tests/runtime/run-lhiver-runtime-test-macos.sh \
  enemy-heal-combat \
  "$HOME/Library/Application Support/OpenGothic"

tests/runtime/run-lhiver-runtime-test-macos.sh \
  orc-behind-detection \
  "$HOME/Library/Application Support/OpenGothic"
```

The runner configures `OPENGOTHIC_RUNTIME_TESTS=ON` in the isolated
`build-runtime-tests` directory, builds OpenGothic, runs from the Gothic
installation directory, waits for the loaded world and runtime fixture, records
only the exact PID-matched Gothic window, and validates the result
independently. The normal `build` directory remains a release-style build
without the harness.

Environment overrides:

- `OPENGOTHIC_BINARY`
- `OPENGOTHIC_RUNTIME_BUILD_DIR` (default `build-runtime-tests`)
- `OPENGOTHIC_MOD` (default `Buddygoths_LhiverUriziel.ini`)
- `OPENGOTHIC_SAVE` (default `5`)
- `OPENGOTHIC_RECORD` (`1` by default; set to `0` for JSON/log only)
- `OPENGOTHIC_RECORD_DURATION` (defaults to 35 seconds for the Orc test and
  50 seconds for the enemy-heal test)
- `OPENGOTHIC_BUILD_JOBS` (default `8`)
- `OPENGOTHIC_READY_TIMEOUT` (default `1200` seconds, including first-run
  Metal shader compilation)
- `OPENGOTHIC_OSX_SYSROOT` (normally discovered with `xcrun`)

Generated JSON, log, MOV, and preview PNG files are stored under
`tests/runtime/artifacts/<date>-lhiver-uriziel/` and intentionally ignored by
Git.

Existing JSON results can be checked without launching the game:

```sh
python3 tests/runtime/verify-results.py \
  tests/runtime/artifacts/2026-07-27-lhiver-uriziel/*.json
```

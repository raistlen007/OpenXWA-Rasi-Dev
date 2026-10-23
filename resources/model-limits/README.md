# Model limits: mission-space test branch

This branch tests the original X-Wing Alliance flight-group starting-coordinate
fields with an oversized Mon Calamari Cruiser. The engine uses real mission
coordinates and its normal mission loader and spawn logic. The renderer does
not reposition either object for these tests.

## Run on Windows

1. Fetch and check out `model-limits-mission-test` in SourceTree.
2. Compile OpenXWA with your normal build workflow.
3. Launch the compiled build, selecting your existing, **unchanged** original
   X-Wing Alliance installation when prompted for game data.
4. Open Combat Simulator and click **MODEL LIMITS** on the main Combat
   Simulator screen. This opens the first test in Mission Setup. Use the
   previous/next mission arrows to select the distance, then launch the mission.
5. During flight, open the debug overlay with backquote and select
   `Model Limits`. Record the player's and cruiser's actual positions,
   distance, render-region status, and whether the HD mesh loaded.

There is **no installation step** and no separate model file to copy. The
exact `ZTest.opt` is committed in this branch at
`resources/model-limits/flightmodels/calamaricruisernew.opt`.
The CMake build copies it into the executable's own resource directory and
generates the eleven missions into `resources/model-limits/combat`.

## Asset isolation

In this branch only, read-only requests for `Combat/mission.lst`,
`Combat/ZTest_*.tie`, and `FlightModels/CalamariCruiserNew.opt` resolve
to the compiled package's resources. The modern HD OPT converter reads the
same bundled cruiser model. All other game assets are loaded normally from
your original XWA installation.

The Combat Simulator mission list is intentionally replaced **in memory**
for this experimental build; the installed `Combat/mission.lst` stays
untouched. No scripts modify or back up any original game files.

## Test cases

The signed 16-bit mission starting positions have 6.25 metres per unit:

- 150, 180, 220, 260, 300, 350, and 400 km axial Z separations, with
  a 10 km lateral observer offset.
- X or Z axis at opposite signed extremes: about 409.6 km separation.
- X/Z diagonal extremes: about 579.2 km.
- X/Y/Z diagonal extremes: about 709.4 km.

The cruiser spans roughly 1,364 km, so the observer may still be within
its geometry even at these center-to-center distances.

The snapshot debug window reports the actual spawn positions and whether
the HD mesh was loaded; **it cannot prove the mesh produced visible pixels**.
The missions have been structurally checked but not launched in a game.

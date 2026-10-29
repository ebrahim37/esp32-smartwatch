# Router and placer evaluation

Installed KiCad10.0.7, pcbnew on system Python3.14. Original watch remains untouched. Checkpoint `checkpoints/pre-39mm-redesign.tar.gz` retains the previous141-footprint50F/91B board,1416 copper items,15zones and all previous scripts/logs. New authoritative unit board remains `design/watch.kicad_pcb`.

## Custom placement and interrupted maze router

`place_revision.py` loads the current companion board, not the old unplaced watch backup. It flips all parts to F.Cu, fixes perimeter/module/switching anchors and packs occupied envelopes on0.25mm sites. The actual82×38.5 board fitted129 footprints, no bottom parts. Actual KiCad placement DRC had zero copper/courtyard violations; independent KRT assembly audit returned zero blocking or advisory pairs. The source electrical graph is unchanged apart from twelve removed test points. Two retained EN/BOOT pads are0.6mm exposed copper without paste.

The interrupted `maze.cpp` now has an actual integration `maze_revision.py`: obstacle and full-depth-via raster construction, F/In2/B A*, protected power/RF exclusions, real KiCad DRC-oracle acceptance and rollback. Compiled with g++ and exercised on a blocked-F/B case requiring a layer transition; observed alternate-layer route,212expanded states. This smoke proves the backend path/layer behavior, not whole-board completion. Whole-board repair results, if invoked, are in `routing/maze-revision/`.

## FreeRouting

Historical native2.5 DSN/SES export-import worked but left opens;2.1 repair previously failed its modified-DSN parser. The new benchmark uses **native pcbnew ExportSpecctraDSN**, FreeRouting2.5 and the same legal revision placement, isolated from the authoritative board. It parsed correctly (the old2.1 parser failure was not repeated). At900s watchdog it still had77best internal unrouted items and had not written an SES. Log: `routing/freerouting-benchmark/runner.log`; exact termination `routing/result-revision.json`. Internal zero-violation metrics are NOT KiCad DRC results. No unverified FreeRouting result was adopted.

## KiCadRoutingTools

Source https://github.com/drandyhaas/KiCadRoutingTools . Direct CLI help/README/schema/dependencies inspected. Native Linuxx64 Rust module v0.23.7 installed using build_router.py; imports work on system Python3.14 and KiCad10. No paid AI API, Claude session or separate authentication is required for these algorithms.

* place_simple.py: functional groups and independently declared floorplan tested with98 movable parts, seed3,50000moves per anneal. Two anneals produced an output, but its own independent assembly grade said NOT BUILDABLE and floorplan had errors. It was not adopted. Custom legal placement retained; no courtyard waivers added to excuse the optimizer.
* route_planes.py: existing In1/F/B GND fills retained and actual stitching/fence generated. Direct1:1net/layer arguments required repeating GND three times; first CLI syntax attempt was corrected.
* route.py: actual power run saved a board; nine of ten named power nets completed at that step, one ESP inductor connection remained open. KiCad after refill reported0copper errors,1dangling-via warning,123opens and2TP field discrepancies. Routing summaries include narrow pad-entry branches: they are not a blanket ampacity approval. The inherited placer had exchanged the functional locations of L1(.47uH,TPS63802) and L3(1uH,BQ25606); this is a placement bug, not a schematic error. `repair_inductor_placement.py` corrects the locations using actual pad-net assertions.
* route_diff.py: first test found the USB pair, solved0.2144mm width/0.15mm gap on the actual0.2104mm dielectric (~89.9Ω model), but failed coupled fanout/polarity geometry and committed a partially connected single-ended fallback. Exit0 did NOT mean a valid USB result. Its first report must not be described as matched or complete. USB array/controller placement refinement and further independent KiCad checks are required.
* check_connected/check_drc/check_assembly, actual CLI KiCad DRC and calibrated3D/GLB height readers are used as separate checks. The router's skipped conditional .dru rules remain enforced by KiCad.

All route calls explicitly use --no-fix-drc-settings, --escalation off and standard fabrication floors. No severity relaxation or clearance ceiling is used; no polarity or GPIO swaps are authorized. The router may still produce pad-entry neckdowns and via-in-pad sites; these require explicit review and filled/capped manufacturing instructions, not a false claim of ordinary tented-via assembly.

Decision: retain the custom legal placement; use KRT for direct planes and routing, with real KiCad acceptance and custom obstacle/DRC-oracle repair as needed. FreeRouting did not produce an importable benchmark result. Final handoff/DRC, not this intermediate evaluation, determines completion and ordering status.

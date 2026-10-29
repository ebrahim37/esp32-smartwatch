# Watch4 hardware

Two independent KiCad projects. Open the project file, not a loose historical PCB.

| Location | Purpose |
|---|---|
| `kicad/watch/watch.kicad_pro` | Original smartwatch; circuitry and layout preserved |
| `kicad/companion/companion.kicad_pro` | Companion carrier; authoritative PCB, root schematic, hierarchy, rules and local libraries |
| `manufacturing/watch/`, `manufacturing/companion/` | Validated, versioned manufacturing releases only |
| `firmware/watch/`, `firmware/companion/` | Separate future firmware workspaces; no firmware supplied |
| `datasheets/` | Shared manufacturer documents |
| `agent/companion/scripts/` | Routing, mechanical, validation and export tools |
| `agent/companion/research/` | Stackup, assembly, sourcing and antenna evidence |
| `agent/companion/reports/` | Measured engineering results; timestamps and source design matter |
| `agent/companion/routing/`, `tools/` | Scratch routing states and third-party tooling |
| `agent/companion/checkpoints/`, `backups/`, `archive/` | Recovery data and historical/unvalidated release attempts |

## Current companion state

Saved geometry: **82 × 38.5 × 0.8mm**, four copper layers, **129 top footprints / zero bottom components**. Routing is complete. See [current measured counts and input hashes](agent/companion/reports/final-verification.json) and the [manufacturing-readiness report](agent/companion/reports/manufacturing-readiness.md), not old routing logs.

The final companion copper checks have **zero errors, zero warnings, zero unconnected items and zero schematic-parity findings**. ERC has zero errors and **233 unsuppressed warnings involving imported Unspecified pins**; it is not a fully typed circuit sign-off. The watch retains its original unrouted layout and rule coverage, including ignored pin-to-pin ERC; it is not a companion release candidate.

Via minimization moved 71 overlapping holes to off-pad dogbones. **54 retained via-in-pad features, ten centred, require filled/capped construction**; all are explicitly marked and individually mapped in the generated `via-in-pad-map.csv`. This is a real four-layer manufacturing surcharge/DFM gate, not ordinary tenting. No manufacturer process approval or price has been obtained.

## KiCad workflow

KiCad 10.0.7 is the currently exercised version. Install standard KiCad libraries and 3D models. Project-specific symbols, footprints and models are local to each project; do not copy hierarchical sheets between projects.

From the repository root:

```sh
kicad kicad/watch/watch.kicad_pro
kicad kicad/companion/companion.kicad_pro
/usr/bin/python3 agent/companion/scripts/maze_revision.py --smoke
kicad-cli pcb drc --refill-zones --save-board --schematic-parity --severity-all --format json -o "$(pwd)/agent/companion/reports/current-drc.json" "$(pwd)/kicad/companion/companion.kicad_pcb"
kicad-cli sch erc --severity-all --format json -o "$(pwd)/agent/companion/reports/current-erc.json" "$(pwd)/kicad/companion/companion.kicad_sch"
/usr/bin/python3 agent/companion/scripts/verify.py
/usr/bin/python3 agent/companion/scripts/assembly.py
/usr/bin/python3 agent/companion/scripts/write_handoff.py
/usr/bin/python3 agent/companion/scripts/release_revision.py
```

Automation uses repository-relative paths derived from its script location. Python tools require the system `pcbnew` module, NumPy, SciPy and Shapely; the maze backend additionally needs a C++20 compiler. KiCad CLI parity checks use absolute input paths because this installed build asserts on relative schematic paths.

Mechanical verification uses the retained KiCadRoutingTools checkout. To restore it in a fresh clone:

```sh
git clone https://github.com/drandyhaas/KiCadRoutingTools.git agent/companion/tools/KiCadRoutingTools
git -C agent/companion/tools/KiCadRoutingTools checkout 05e92a9ca236a5b70033c0eb2505bb3e1147976a
```

Install that tool's Python dependencies for the same interpreter. FreeRouting is optional; its JARs are not supplied and its launcher reports the exact required install path. The proven maze backend does not require FreeRouting.

The maze router modifies the canonical companion PCB only after an actual refill/DRC accepts the candidate. Older placement, testpoint-removal and power-geometry scripts are historical transformations, not a rebuild command: do not rerun them casually over the routed design. Checkpoints are retained locally and ignored by Git to avoid committing large generated histories.

## Manufacturing policy

No file under an old `jlcpcb_release*` directory is authoritative. Those attempts are archived. `agent/companion/scripts/release_revision.py` generates **only an engineering candidate** under `agent/companion/archive/release-candidate/rev-a/`, not a published release. It contains unit Gerbers/drills, sourced BOM/CPL, assembly drawings, top/bottom renders, stackup/mechanical notes, verification reports and a SHA-256 manifest. `release-status.json` and `ORDERING-STATUS.txt` explicitly retain `ready_for_ordering: false`.

`manufacturing/companion/rev-a/` is deliberately **not published**: supplier POFV/overhang DFM, stock allocation, BOM/CPL/rotation preview, and engineering acceptance remain incomplete. JLCPCB must provide Standard-PCBA panelization/fixture around the unit board's antenna overhang and connector openings; no panel PCB or panel Gerbers are supplied. R19 now uses a verified SMT-classified Vishay equivalent; C39/C68 manufacturer identity is confirmed by LCSC, but neither identity nor retail availability reserves PCBA stock.

The independent fabrication parser matches every plated/nonplated hole and slot centre/size and all 123 fitted placements to the native PCB, and checks shared Gerber/drill/CPL origin alignment. See the generated candidate's `coordinate-verification.json`. These byte-level checks and inspected renders do not substitute for supplier DFM or hardware qualification.

DRC does not establish power capacity, USB/RF compliance, battery swelling safety, enclosure tolerances, or loaded thermal performance. Via-in-pad needs explicit filled/capped process review and a four-layer quote. Current verification and outstanding gates are recorded in `agent/companion/reports/` and the candidate readiness report when generated.

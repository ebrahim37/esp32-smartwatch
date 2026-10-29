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

## KiCad workflow

KiCad 10.0.7 is the currently exercised version. Install standard KiCad libraries and 3D models. Project-specific symbols, footprints and models are local to each project; do not copy hierarchical sheets between projects.

From the repository root:

```sh
kicad kicad/watch/watch.kicad_pro
kicad kicad/companion/companion.kicad_pro
/usr/bin/python3 agent/companion/scripts/maze_revision.py --smoke
kicad-cli pcb drc --refill-zones --save-board --schematic-parity --severity-all --format json -o "$(pwd)/agent/companion/reports/current-drc.json" "$(pwd)/kicad/companion/companion.kicad_pcb"
kicad-cli sch erc --severity-all --format json -o "$(pwd)/agent/companion/reports/current-erc.json" "$(pwd)/kicad/companion/companion.kicad_sch"
```

Automation uses repository-relative paths derived from its script location. Python tools require the system `pcbnew` module, NumPy, SciPy and Shapely; the maze backend additionally needs a C++20 compiler. KiCad CLI parity checks use absolute input paths because this installed build asserts on relative schematic paths.

The maze router modifies the canonical companion PCB only after an actual refill/DRC accepts the candidate. Older placement, testpoint-removal and power-geometry scripts are historical transformations, not a rebuild command: do not rerun them casually over the routed design. Checkpoints are retained locally and ignored by Git to avoid committing large generated histories.

## Manufacturing policy

No file under an old `jlcpcb_release*` directory is authoritative. Those attempts are archived. `agent/companion/scripts/release_revision.py` generates an engineering candidate under `agent/companion/archive/release-candidate/`, not a published release. Publish to `manufacturing/companion/rev-a/` only after final electrical, geometry, BOM/CPL and manufacturing-process gates pass. No panel design is supplied: JLCPCB must provide Standard-PCBA panelization/fixture around the unit board's antenna overhang and connector openings.

DRC does not establish power capacity, USB/RF compliance, battery swelling safety, enclosure tolerances, or loaded thermal performance. Via-in-pad needs explicit filled/capped process review and a four-layer quote. Current verification and outstanding gates are recorded in `agent/companion/reports/` and the candidate readiness report when generated.

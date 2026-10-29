# Companion portability and library audit

Read-only dependency inspection: 2026-10-29. This report is not a DRC, ERC, routing, or mechanical-clearance pass. No placement generator, router, PCB save, schematic transformation, build, test, linter, or formatter was executed for this audit. Library path-metadata repairs and the automation changes below were applied separately; no circuitry, symbol pins, placement, or copper was changed by this work.

## Canonical paths and automation changes

- Canonical original: `kicad/watch/watch.kicad_pro`, `.kicad_sch`, and `.kicad_pcb`.
- Canonical companion: `kicad/companion/companion.kicad_pro`, `.kicad_sch`, and `.kicad_pcb`.
- Automation derives the repository and artifact roots from `__file__`; this work introduces no absolute repository-location dependency.
- `scripts/run_router.py` no longer depends on ephemeral `/tmp` JARs. It requires the real release JAR at `agent/companion/tools/freerouting-2.5.0.jar`, or `freerouting-2.1.0.jar` for `--repair`. Missing JAR, Java runtime, and matching DSN input each produce an explicit prerequisite error before starting a process. No substitute router or automatic download was added.
- `scripts/mechanical_revision.py` uses the artifact-relative `tools/KiCadRoutingTools` checkout, checks both imported reader entry points, reports missing Python dependencies, and requires `kicad-cli` for actual GLB extraction. Model-directory environment overrides take precedence over the installation-relative KiCad model directory. Referenced but absent model files now stop the measurement with their reference and resolved path instead of being silently skipped.
- `scripts/run_krt_revision.py` checks the selected real tool entry point and uses the active Python interpreter. `scripts/compare_freerouting.py` also uses the active interpreter for the runner.
- `scripts/functional-groups.json` now identifies `companion.kicad_pcb`.
- The historical 30 mm `scripts/place.py` reads the preserved canonical watch board instead of a nonexistent extracted `backups/source-kicad` directory. Its existing output remains the companion board. **It is a destructive historical placement generator, not a continuation command for the validated all-top design; it was not executed.** Historical report/checkpoint filenames were not renamed merely because they contain old design names.
- Companion project metadata observed during inspection identifies `companion.kicad_pro` and `companion.kicad_sch`; its project-instance names are `companion`. Original watch project-instance names remain `watch`. The main migration owner made the project-metadata changes, not this audit worker.

## Dependency-resolution measurements

S-expression inspection covered both boards, all eight schematic files per project, local symbol definitions, footprint tables, assigned footprint IDs, local footprint model declarations, and the installed standard KiCad library locations. `${KIPRJMOD}` was resolved relative to the owning canonical project. The host has standard libraries under `/usr/share/kicad`; its KiCad 10 global tables include the installed default library tables. No shell `KICAD*_3DMODEL_DIR` overrides were present.

| Measured item | Original watch | Companion |
|---|---:|---:|
| Saved board footprints | 141 | 129 |
| Distinct board footprint IDs | 29 | 29 |
| Unresolved board footprint IDs | 0 | 0 |
| Distinct instantiated symbol IDs across all source sheets | 33 | 33 |
| Unresolved instantiated symbol IDs | 0 | 0 |
| Instantiated symbols absent from their sheet's embedded cache | 0 | 0 |
| Distinct schematic-assigned footprint IDs across all source sheets | 35 | 35 |
| Saved board model references | 126 | 127 |
| Missing files among saved board model references | 0 | 0 |
| Board models using project-local paths | 29 | 30 |
| Board models using `KICAD10_3DMODEL_DIR` | 97 | 97 |

Both root schematics contain five direct child-sheet file references; all five resolve within their project. The display and sensors/haptics source sheets are also present but are not referenced from either root schematic. This is an observation, not a hierarchy or circuitry repair.

Required standard symbol libraries are `Connector`, `Device`, and `power`; the custom `libs` symbol library resolves through each project's `${KIPRJMOD}/libs/jlcpcb.kicad_sym`. Required board footprint families are `Capacitor_SMD`, `Capacitor_Tantalum_SMD`, `Resistor_SMD`, `libs`, and, on the original board, `TestPoint`. Watch custom footprints resolve to its own `libs/jlcpcb.pretty`; companion table entries resolve to its own `layout-footprints/*.pretty`. No companion dependency points into the watch project.

### Library metadata repairs

The initial audit found undefined `jlcpcb:` footprint nicknames in the two vendor symbol libraries: **29 Footprint properties representing 28 distinct target IDs in each library**. Their exact S-expression property nodes were identified with `scripts/sexp_spans.py`; only the nickname was migrated to the existing canonical `libs:` namespace. Pin definitions and symbol circuitry were not edited. All 28 distinct defaults were resolved against each project's actual footprint-table target; none remains missing. No compatibility nickname or global-library alias was added.

The companion's artwork-adjusted libraries originally contained only board-used definitions. Consequently six distinct schematic assignments on the unreferenced display/sensor sheets did not resolve through its shadowing tables. To retain a complete local library without changing existing artwork-adjusted footprints, the following previously absent definitions were added to `layout-footprints/libs.pretty` from the companion's own `libs/jlcpcb.pretty`:

- `BULETM-SMD_ESP32-PICO-MINI-02U`
- `LGA-12_L2.0-W2.0-P0.50-BL`
- `VSSOP-10_L3.0-W3.0-P0.50-LS4.9-BL`
- `CONN-SMD_2P-P1.00_SM02B-SRSS-TB-LF-SN`
- `FPC-SMD_19P-P0.60_DIANWEI_8.03A0.019200`
- `VLGA-12_L2.0-W2.0-P0.50-BL`

The first entry also completes the vendor library's otherwise-unused default. `C_0805_2012Metric.kicad_mod` was added to `layout-footprints/Capacitor_SMD.pretty` from the installed standard KiCad footprint library for the standalone sensors sheet. All seven additions were absent beforehand; no existing land pattern was overwritten. Each added definition's referenced model file was resolved before copying. These additions close all six initially missing schematic-assignment IDs and the unused vendor default without modifying a schematic assignment.

Before these additions, all 27 model declarations in the watch's table-selected local footprints and all 28 declarations in the companion's table-selected local footprints resolved. The seven added footprints also reference available models. Original watch U10 has no model; its fourteen test points likewise have none. Companion TP9 and TP10 have no model. The companion U10 model is explicitly a documented mechanical envelope, not a detailed vendor solid.

## Outstanding prerequisites and limitations

1. **Both FreeRouting JARs are absent** from the permanent tool directory. Java and `kicad-cli` are available on this host, and `routing/companion.dsn`, `routing/revision.dsn`, and `routing/repair.dsn` are present. Install the real version-matched upstream JAR(s) at the documented paths before using `run_router.py`. Their execution and Java-version compatibility were not tested.
2. **Missing datasheet:** `FH_0402CG121J500NT_C40059_MLCC.pdf` was not found in the repository datasheets directory. The companion's corrected `${KIPRJMOD}/../../datasheets/...` link therefore still has no target. The original watch retains its historical `${KIPRJMOD}/../datasheets/...` link, which also does not resolve after nesting; it was intentionally not rewritten because original circuitry/layout files must remain byte-identical. This is an optional documentation dependency, not proof of an electrical defect. No PDF was fabricated.
3. Standard KiCad symbols, footprints, and the **KiCad 10 3D-model library** remain installation prerequisites; availability on this host does not vendor those packages into the repository. On another machine, configure KiCad's standard libraries and matching `KICAD10_3DMODEL_DIR` (or install their normal defaults). Older-version compatibility was not exercised.
4. Real `pcbnew`, NumPy, Shapely, and the relevant scripts' SciPy dependencies must be installed in the selected Python interpreter. The KiCadRoutingTools reader source files exist; its complete Python/native runtime dependency set was not executed or certified by this static audit.
5. DRC/ERC warnings, connectivity, fabrication acceptance, GLB-height calibration, and final routing quality are outside this report. No existing warning was suppressed, no symbol re-pinned, and no rule severity changed.

## Original-watch preservation evidence

Direct byte comparison against `checkpoints/watch4-before-migration-20261029.tar.gz` found the original watch PCB and all eight original schematics **byte-identical**. The sole original-watch change made by this work is the intentional vendor-symbol-library Footprint nickname metadata migration described above; do not interpret that library file as byte-identical to the archive.

| Preserved file | SHA-256 |
|---|---|
| `watch.kicad_pcb` | `34537c797674c974e4f336f6c4594f200367eb58f890feeeec81bf1356a3ddca` |
| `watch.kicad_sch` | `a3432f1ac2fce897d38736001f3b34d1170da33550803331ad6e70136840d77b` |
| `01_power.kicad_sch` | `52a43a654dddf1ecd5452bb70674005b008ad7e7a34a523d04f02908e2c17bd9` |
| `02_esp32.kicad_sch` | `01ed533abce3ef86abd53be6e4d8b0b6cf789fc931f36cd69da360582821fc94` |
| `03_usb.kicad_sch` | `d7bd617ddca93c0e81c7ce1cff3cbf11debb2db5e1ddb95bdcdfb79c4a6ababf` |
| `04_modem.kicad_sch` | `b6077ac6a10f212d100392efe5353e3118be2a9050feb3125a2907737a12a6d0` |
| `05_display.kicad_sch` | `8132382e030536f085223bd35d3f11a6aede55f9c7ec9d5da8a8d99a9e6e1f82` |
| `06_storage.kicad_sch` | `48a10dbf0dea635968486268601e17734ad46156d020d6c50fa85f8ba956deac` |
| `07_sensors_haptics.kicad_sch` | `ea2086b88ebcf2504588799e00fbb7bc432c254badaa34e2ebb94d8d2926d82c` |

Final CLI verification belongs to the main migration/routing owner. The evidence above is limited to file/path resolution and the explicitly recorded archive byte comparisons.

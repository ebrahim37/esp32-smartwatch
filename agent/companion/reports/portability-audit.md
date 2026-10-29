# Companion portability and library audit

Read-only dependency inspection: 2026-10-29. This report is not a DRC, ERC, routing, or mechanical-clearance pass. No placement generator, router, PCB save, build, test, linter, or formatter was executed by this audit worker. Deliberate schematic/library metadata repairs and the automation changes below were applied separately; no actual-instance symbol pins, circuitry, placement, or copper was changed by this work.

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

S-expression inspection covered both boards, all eight schematic files per project, local symbol definitions, footprint tables, assigned footprint IDs, local footprint model declarations, and the installed standard KiCad library locations. `${KIPRJMOD}` was resolved relative to the owning canonical project. The host has standard libraries under `/usr/share/kicad`; its KiCad 10 global tables include the installed default library tables. No shell `KICAD*_3DMODEL_DIR` overrides were present. The table records the initial audit snapshot; the final companion SN74LVC2G126 defaults were reconciled to the existing canonical board footprint as described below.

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

The initial audit found undefined `jlcpcb:` footprint nicknames in the two vendor symbol libraries: **29 Footprint properties representing 28 distinct target IDs in each library**. Their exact S-expression property nodes were identified with `scripts/sexp_spans.py`; only the nickname was migrated to the existing canonical `libs:` namespace. All 28 initial defaults were resolved against each project's actual footprint-table target. Matching cached vendor symbols in each project's seven child schematics also received **29 embedded Footprint-default nickname changes per project**. Actual symbol-instance footprint assignments, pins, values, UUIDs, wires, and labels were not changed by this cache normalization. No compatibility nickname or global-library alias was added.

The companion's artwork-adjusted libraries originally contained only board-used definitions. Consequently six distinct schematic assignments on the unreferenced display/sensor sheets did not resolve through its shadowing tables. To retain a complete local library without changing existing artwork-adjusted footprints, the following previously absent definitions were added to `layout-footprints/libs.pretty` from the companion's own `libs/jlcpcb.pretty`:

- `BULETM-SMD_ESP32-PICO-MINI-02U`
- `LGA-12_L2.0-W2.0-P0.50-BL`
- `VSSOP-10_L3.0-W3.0-P0.50-LS4.9-BL`
- `CONN-SMD_2P-P1.00_SM02B-SRSS-TB-LF-SN`
- `FPC-SMD_19P-P0.60_DIANWEI_8.03A0.019200`
- `VLGA-12_L2.0-W2.0-P0.50-BL`

The first entry also completes the vendor library's otherwise-unused default. `C_0805_2012Metric.kicad_mod` was added to `layout-footprints/Capacitor_SMD.pretty` from the installed standard KiCad footprint library for the standalone sensors sheet. All seven additions were absent beforehand; no existing land pattern was overwritten. Each added definition's referenced model file was resolved before copying. These additions close all six initially missing schematic-assignment IDs and the unused vendor default without modifying a schematic assignment.

Before these additions, all 27 model declarations in the watch's table-selected local footprints and all 28 declarations in the companion's table-selected local footprints resolved. The seven added footprints also reference available models. Original watch U10 has no model; its fourteen test points likewise have none. Companion TP9 and TP10 have no model. The companion U10 model is explicitly a documented mechanical envelope, not a detailed vendor solid.

### Embedded/local symbol consistency

After nickname normalization, a structural comparison of all 29 cached vendor definitions per project found only the three pre-existing inductor-artwork differences: `IHLP2525CZER6R8M01`, `PCT252010-R47-M`, and `XRIM201612S1R0MBCA` (L4, L1, and L3). Their pins and Value fields already agreed. Each local library was reconciled to its own schematic cache by copying only the three inductors' four arc coordinate triplets, including the cached midpoint precision. No schematic pin definition or Value was changed; all embedded artwork remains as originally saved.

The comparison normalizes decimal spelling/negative zero, unordered symbol-member serialization, reversed polyline direction, KiCad-version default flags, legacy property IDs, property-display positions, and legacy `ki_description` versus current `Description`. It compares property values, unit contents, graphical coordinates, and full pin definitions; it is not a claim of byte-identical serialization or a substitute for native ERC.

The earlier decision to synchronize the companion SN74LVC2G126DCUR default to `LS4.9-BL` was superseded by the main owner's native measurement of canonical U9's actual FPID: `libs:VSSOP-8_L2.3-W2.0-P0.50-LS3.1-BR`. Both the companion embedded library default and its local library default now use that existing `LS3.1-BR` ID, consistent with the already-correct actual schematic instance and saved board. No `LS4.9-BL` footprint was invented or exported. The watch project was unchanged by this correction.

The requested structural comparison before this final default correction compared **29 cached vendor definitions in watch and 29 in companion, with zero normalized mismatches**. The final correction changes the same single Footprint-default value in both companion definitions, preserving their equality and removing the previously reported nonexistent `LS4.9-BL` target. The existing `LS3.1-BR` target resolved in the initial path audit; there is no outstanding companion footprint prerequisite from that superseded decision. Native final ERC remains the main owner's responsibility.

### Sourcing metadata cutover

At the main owner's explicit request, only these actual companion symbol-instance sourcing fields were changed:

- C39 and C68: `Manufacturer` = `Kyocera AVX`, retaining existing values, LCSC IDs, and other fields. Main-provided primary reference: [LCSC C148107](https://www.lcsc.com/product-detail/C148107.html), identifying TPSB107M010R0400.
- R19: `LCSC Part` = `C2117579`, `MPN` = `MCS04020C5492FE000`, `Manufacturer` = `Vishay Intertech`. Its existing `54.9k` value and `Resistor_SMD:R_0402_1005Metric` footprint assignment were retained. Main-provided sourcing reference: [JLC SMT C2117579](https://jlcpcb.com/partdetail/VishayIntertech-MCS04020C5492FE000/C2117579), a 54.9 kΩ, 1%, 0402 replacement.

The main owner verified the linked product identities and owns matching catalog/PCB-field synchronization. These changes make no price, stock, or allocation claim.

## Outstanding prerequisites and limitations

1. **Both FreeRouting JARs are absent** from the permanent tool directory. Java and `kicad-cli` are available on this host, and `routing/companion.dsn`, `routing/revision.dsn`, and `routing/repair.dsn` are present. Install the real version-matched upstream JAR(s) at the documented paths before using `run_router.py`. Their execution and Java-version compatibility were not tested.
2. **Missing datasheet:** `FH_0402CG121J500NT_C40059_MLCC.pdf` was not found in the repository datasheets directory. The companion's corrected `${KIPRJMOD}/../../datasheets/...` link therefore still has no target. The original watch retains its historical `${KIPRJMOD}/../datasheets/...` link, which also does not resolve after nesting; it was intentionally not rewritten because the original circuit and optional documentation fields were outside the authorized cache-metadata migration. This is an optional documentation dependency, not proof of an electrical defect. No PDF was fabricated.
3. Standard KiCad symbols, footprints, and the **KiCad 10 3D-model library** remain installation prerequisites; availability on this host does not vendor those packages into the repository. On another machine, configure KiCad's standard libraries and matching `KICAD10_3DMODEL_DIR` (or install their normal defaults). Older-version compatibility was not exercised.
4. Real `pcbnew`, NumPy, Shapely, and the relevant scripts' SciPy dependencies must be installed in the selected Python interpreter. The KiCadRoutingTools reader source files exist; its complete Python/native runtime dependency set was not executed or certified by this static audit.
5. DRC/ERC warnings, connectivity, fabrication acceptance, GLB-height calibration, and final routing quality are outside this report. No existing warning was suppressed, no symbol re-pinned, and no rule severity changed.

## Original-watch preservation evidence

Direct comparison against `checkpoints/watch4-before-migration-20261029.tar.gz` confirms the original watch PCB and root schematic remain **byte-identical**. The seven child schematics intentionally differ only by the embedded Footprint-default namespace changes listed below. Reversing **only** those cache-property strings from `libs:` back to `jlcpcb:` reproduces each original checkpoint file byte-for-byte. This exact normalized-byte comparison proves that every other byte—including actual-instance fields, circuitry, pins, wires, UUIDs, values, and graphics—is preserved. The watch vendor symbol library intentionally differs through its Footprint-default nickname migration and three-inductor artwork synchronization.

The following SHA-256 values identify the **checkpoint originals** (also the normalized current bytes); child-schematic current hashes differ because their cache metadata was intentionally migrated.

| Checkpoint/normalized file | SHA-256 |
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

| Watch child schematic | Embedded default strings changed | Current SHA-256 |
|---|---:|---|
| `01_power.kicad_sch` | 10 | `c3c936c3160f2abe76e72f2511cc857b06c197e3e563f4591f334baf6f816b7c` |
| `02_esp32.kicad_sch` | 1 | `3aea5b22e7aef0b2d7c7e19f559e6a09e441e8f1b45140db508639a9cc2f8c6d` |
| `03_usb.kicad_sch` | 6 | `54d6ef22943d23d55f38088680a81878f44f4ca80a828f9ccc428e06d2ffc916` |
| `04_modem.kicad_sch` | 6 | `42805b99d8be84a9681fcd547d90771f24118a7c19d14f6cd9be0e3757c40794` |
| `05_display.kicad_sch` | 1 | `f16deb418e2e3b36d9cc1b482e5e5d59ced86b907e9756a78f9318152d46e0e2` |
| `06_storage.kicad_sch` | 1 | `a017fc707383569ea82e4c94fd48ceee845a4aff67e5a6905cccd04a76240a6f` |
| `07_sensors_haptics.kicad_sch` | 4 | `9e2b92678347bc2788a84cadebd86dff330733010af034a0bf3265da000a99b1` |

Final CLI verification belongs to the main migration/routing owner. The evidence above is limited to file/path resolution and the explicitly recorded archive byte comparisons.

## Parent completion updates

The previously missing FH C40059 document was retrieved through the [LCSC part record](https://www.lcsc.com/product-detail/C40059.html) and its [actual PDF](https://datasheet.lcsc.com/datasheet/pdf/09fe2daf22580a99fa7fea93ccb20fb7.pdf?productCode=C40059), saved as `datasheets/FH_0402CG121J500NT_C40059_MLCC.pdf`, identified as PDF and opened as the FH general-series MLCC specification. Watch C29's schematic and PCB Datasheet fields were corrected from `${KIPRJMOD}/../datasheets/` to `${KIPRJMOD}/../../datasheets/` to match its relocated depth. These are documentation metadata changes, not circuitry or layout changes. The original-watch PCB is therefore no longer byte-identical *before* this one path normalization; `finalize_migration.py` explicitly normalizes that field plus embedded default nicknames before comparing original circuit/layout bytes.

The companion SN74LVC2G126DCUR default was resolved to the **measured actual U9 FPID** `libs:VSSOP-8_L2.3-W2.0-P0.50-LS3.1-BR`, in both cache and local symbol library. Its real board land pattern was exported under that exact ID; no nonexistent LS4.9 footprint or compatibility alias was created.

The two unused companion copies of watch-only `05_display` and `07_sensors_haptics` were moved into `agent/companion/archive/unused-watch-sheets/`; neither is referenced by the companion root. Both remain active and independent in the watch project.

Final native ERC/DRC, parity, geometry, and source hashes are recorded by `scripts/verify.py` in `reports/final-verification.json`, superseding earlier report counts. KiCadRoutingTools provenance: `https://github.com/drandyhaas/KiCadRoutingTools.git`, revision `05e92a9ca236a5b70033c0eb2505bb3e1147976a`. Its retained checkout is ignored by Git and can be restored at that exact revision.


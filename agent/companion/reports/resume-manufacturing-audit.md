# Resume ERC and manufacturing audit

Read-only audit, 2026-10-29. No schematic, PCB, rules, BOM, or panel files were modified or generated. Current inputs: `design/watch.kicad_sch`, `design/watch.kicad_pcb`, `reports/resume-erc.json`, `reports/resume-netlist.xml`, `jlcpcb_release-revision/bom-detailed.csv`, and the saved manufacturer evidence in `research/manufacturing.md` / `research/catalog.json`.

## Fresh ERC and netlist

Installed KiCad CLI is **10.0.7**. `kicad-cli sch erc --help` exposes JSON/report output, `--severity-all`, individual severity selectors, and `--exit-code-violations`; `kicad-cli sch export netlist --help` supports KiCad XML (`kicadxml`). Fresh exports were made from the current project-root schematic using `--format json --severity-all` and `--format kicadxml`:

- ERC: `reports/resume-erc.json` (2026-10-29 02:07:49)
- Netlist: `reports/resume-netlist.xml` (2026-10-29 02:07:50)

ERC contained **0 errors, 236 warnings, 0 exclusions**. Exact warning categories:

| ERC type | Count | Description / assessment |
|---|---:|---|
| `pin_to_pin` | 100 | Unspecified ↔ Passive pins connected |
| `pin_to_pin` | 54 | Unspecified ↔ Power input pins connected |
| `pin_to_pin` | 79 | Unspecified ↔ Unspecified pins connected |
| `lib_symbol_mismatch` | 1 each | Local-library copy drift at L1, L3, and L4 |

All 233 `pin_to_pin` warnings include at least one **Unspecified** pin from imported/custom symbols; none reports a typed output-to-output conflict. `pin_to_pin` was **not suppressed** in the fresh report. The three library warnings name `PCT252010-R47-M` (L1), `XRIM201612S1R0MBCA` (L3), and `IHLP2525CZER6R8M01` (L4); reconcile the embedded symbol/library copies when editing those parts, but these warnings alone are not evidence of a bad connection. The report's only ignored checks are `four_way_junction` and `simulation_model_issue`.

**Electrical parity:** parsed the fresh XML component pin/net assignments and compared them to the current PCB's native footprint-pad assignments by `(reference, pin number)`: **586 schematic reference-pads, 586 board reference-pads, 586 exact key matches, 0 net mismatches, 0 schematic-only or board-only pads** (129 board footprints). This establishes schematic-to-board netlist parity, not circuit-intent correctness; no schematic ERC or pad-net mismatch has identified a functional wiring fault. The untyped-pin warnings need symbol pin-type cleanup for a stronger future ERC, not blanket rule suppression.

## Ranked manufacturing findings

### M1 — SERIOUS: 28 pad-centered vias require explicit POFV handling; 11 are wider than their landing pad

The current 4-copper-layer board has **266 vias**, all **0.60 mm pad / 0.30 mm drill**. Native PCB geometry shows **28 via centres exactly coincident with SMT pad centres**. These are via-in-pad features, not ordinary tented vias. Of those, **11 exceed the narrow dimension of their associated SMT pad**; radial annular copper overhang is 0.02–0.10 mm:

`C27.2, QH2.2, R12.1, R11.1, R2.1, C8.1, R35.2, C58.2, C38.2, C22.2, R26.2`.

For example, the 0.60 mm via at `QH2.2` is centered in a 0.40 × 0.70 mm pad (0.10 mm radial overhang); the vias in the 0.54–0.56 mm-wide 0402 lands overhang 0.02–0.03 mm per side. A via in a solder pad can wick solder unless filled and capped; the oversized copper also extends outside the pad geometry. **Before ordering/assembly, reroute these 11 off the lands or have the board owner establish a compliant smaller via/landing design without relaxing board rules.** For any of the remaining 17 pad-centered vias kept, explicitly request JLC POFV (resin/copper-paste fill plus copper cap), verify the drill/pad geometry in the manufacturer's DFM/production-file preview, and obtain a quote. The 0.30 mm drills are inside JLC's published POFV drill range, but that does not waive the pad-size conflict.

JLC's current POFV guide says via-in-pad uses filled-and-capped construction, lists 0.15–0.30 mm as typical drill sizes, minimum mechanical drill 0.15 mm, and minimum pad = drill + 0.15 mm. JLC's current via-covering page says in-pad vias cannot be ink-plugged and filled-via drills should be ≤0.50 mm. JLC's official process update says POFV is free on 6–20 layers but **still chargeable on 4-layer boards**. No current numeric 4-layer VIPPO price was published in the sources checked; **price is not established here**. Do not assume normal tenting/plugging is an adequate substitute for POFV under solder pads.

### M2 — SERIOUS: PCBA must use Standard assembly and supplier panelization

Saved live catalog evidence confirms `U7` (ESP32, C5361945) and `U10` (EG912U, C7498995) are **Standard Only**; U10 also requires X-ray inspection. The 82 × 38.5 mm unit board is below the saved JLC Standard-PCBA 70 × 70 mm minimum dimension, so the existing manufacturing evidence correctly calls for **JLC-supplied panelization/fixture**. Keep the individual unit board at 82 × 38.5 mm and ask JLC to design rails, tabs/fiducials, and routed cavities with antenna and connector-access clearances. **No panel PCB, panel Gerbers, or panelization output was generated**; this respects the user's explicit instruction that JLC handles panelization.

### M3 — MODERATE: assembly and sourcing not yet purchase-ready

The saved BOM has 123 populated positions, 55 distinct LCSC codes, and catalog identity records for those codes, but explicitly says stock is **not reserved**; the live report does not establish current retail or assembly stock or allocation. Re-check availability and the final assembly match at order time; no price is asserted.

- `R19` is a 0402 resistor (C54530922) with an inconsistent **Wave Soldering** supplier classification. Obtain supplier correction/confirmation or plan manual population; do not assume it will be machine-placed as listed.
- `C39` and `C68` (100 µF tantalum, MPN `TPSB107M010R0400`, code C148107) have `Manufacturer=UNVERIFIED` in the BOM. Confirm manufacturer/MPN and polarity against the live supplier record before procurement.
- Nine populated packages are bottom-terminated/no-lead: `QH2`, `QL2`, `U1`, `U3`, `U4`, `U5`, `U8`, `U10`, `U11`. They are not conventional iron-solder-friendly; if any manual assembly is intended, use professional paste/reflow/X-ray capability or establish a documented assembly plan. The BOM also contains **73 populated 0402 positions**, a substantial manual-soldering burden. The BOM itself lists 121 parts as Economic/Standard and the two modules as Standard Only; machine assembly should be planned, not inferred from a component-catalog identity.

## Order-time gates (not completed)

1. Resolve the 11 over-wide pad-centered vias; explicitly specify POFV for any retained via-in-pad features and verify JLC's production-file interpretation and quote.
2. Submit the **unit-board** files to JLC Standard PCBA and request supplier-designed panelization; check rails/tabs clear the ESP antenna and SD/USB openings. No order, panelization, or fabrication-ready declaration is made by this audit.
3. Verify stock, part substitutions, R19 classification, C39/C68 identities, BOM/CPL match, U10 X-ray, and the final price directly in the current supplier quote.

## Sources

- JLCPCB, [Via-in-Pad Design Tutorial and Technical Guide](https://jlcpcb.com/help/article/via-in-pad-pofv-guide), last updated 2026-10-16.
- JLCPCB, [Via Covering: Tented, Untented, Plugged, Epoxy-Filled and Copper-epoxy-filled](https://jlcpcb.com/help/article/pcb-via-covering), last updated 2026-09-09.
- JLCPCB, [Free Via-in-Pad on 6–20 Layer PCBs with POFV](https://jlcpcb.com/news/free-via-in-pad-6-20-layer-pcbs-pofv), explicitly states 4-layer POFV still requires charge.
- JLCPCB, [Panelizing your PCB for Assembly](https://jlcpcb.com/help/article/panelizing-your-pcb-for-assembly), last updated 2026-09-09; standard assembly capability page: [PCB Assembly Capabilities](https://jlcpcb.com/capabilities/pcb-assembly-capabilities).
- JLCPCB, [PCB Assembly Cost](https://jlcpcb.com/help/article/pcb-assembly-price), last updated 2026-10-22; its X-ray explanation covers QFN and other leadless packages. This audit does not estimate the order's cost.
- Project evidence: `research/manufacturing.md` (manufacturer tables captured 2026-10-28), `research/catalog.json`, `reports/assembly.json`, and `jlcpcb_release-revision/bom-detailed.csv`.

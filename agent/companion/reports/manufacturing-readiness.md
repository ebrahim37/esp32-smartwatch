# Companion rev-b manufacturing readiness

**ENGINEERING CANDIDATE — NOT FOR ORDERING.** No validated release has been published into `manufacturing/companion/`. The candidate is `agent/companion/archive/release-candidate/rev-b/`; rev-a and the original JLC reports remain preserved. Supplier process/assembly acceptance and physical qualification remain incomplete.

## Authoritative projects and repository

- Watch: `kicad/watch/watch.kicad_pro`, matching PCB/root schematic and seven child sheets. Original circuitry/layout preserved; only embedded/local symbol metadata reconciled. Watch retains its original ERC rule coverage, including ignored pin-to-pin checking.
- Companion: `kicad/companion/companion.kicad_pro`, `companion.kicad_pcb`, `companion.kicad_sch`, five active child sheets, local libraries and `companion.kicad_dru`. Pin-to-pin ERC is enabled.
- Artifacts: `agent/companion/{scripts,reports,research,routing,tools,checkpoints,backups,archive}`. Old `jlcpcb_release*` outputs and unused companion copies of the watch-only display/sensors sheets are archived, not authoritative. No unique recovery history was deleted.
- Firmware: separate empty `firmware/watch/` and `firmware/companion/`; no unrelated firmware generated.

## Saved-board verification

| Measurement | Result |
|---|---|
| Board | 82 × 38.5 × 0.8 mm; 4 copper layers |
| Footprints | 129; top 129; bottom 0 |
| Native unsupported lands / courtyard overlaps | 0 / 0 |
| In1 GND filled polygons | 1 |
| Companion copper DRC errors / warnings | 0 / 0 |
| Companion unconnected items / schematic parity | 0 / 0 |
| Companion ERC errors / warnings | 0 / 233 |
| Watch DRC errors / warnings / unconnected | 0 / 2 / 384 |
| Watch ERC errors / warnings | 0 / 0 |

Companion ERC categories: `{'pin_to_pin': 233}`. Imported Unspecified pins weaken electrical-type coverage; these warnings were not suppressed. No design-rule limit was relaxed to obtain DRC counts. The watch is preserved, not claimed routed or manufacturing-ready.

## Power and return geometry

All major component poses remain unchanged. The only local passive changes are: C30, C25, C10, C29, C33. C10/C33 moved toward regulator outputs; C25/C30 relocated to preserve input bypass and compensation; C29 flipped within its original courtyard to free its return/COMP escape. The independent review describes the earlier snapshot; these repairs are parent-measured follow-through, not a second independent certification.

BQ25606 SW load copper is now direct F.Cu, with 0.3mm paired pad escapes flaring to 0.6mm, no primary series-via SW detour. PMID to C2 uses a 0.3mm pad escape and 0.6mm local trunk. Bootstrap control routing is separate. VSYS, modem and rail fills retain actual per-terminal via connections; small control/sense branches do not each carry the full rail current. RF launch and regulator-return dogbones were added using actual refill/DRC acceptance.

| Rail DC screen | Applied sink current A | Maximum simulated drop mV | Total simulated copper loss W |
|---|---:|---:|---:|
| /Power/VBAT | 4 | 34.944021078064736 | 0.138454082368979 |
| /Power/VSYS | 4.1 | 39.23737771061595 | 0.15038076748657486 |
| /Power/USB_5V | 2 | 79.71742146219091 | 0.15848816091871015 |
| /Modem/MODEM_3V8 | 3 | 34.441476703595164 | 0.10099436471009587 |
| /ESP32/SYS_3V3 | 0.6 | 29.176640848670225 | 0.01746401839137525 |

The 0.1mm finite-volume screen assumes 35µm outer /15.2µm inner copper and 20µm via-barrel plating. It excludes sub-grid necks and is NOT IPC-2152, electrothermal, capacitor-ESL, switching-stability or current-limit qualification. Battery/modem simultaneous load steps, transmit bursts, regulator overshoot, enclosure temperatures and cell charge-current suitability require hardware tests.

## USB and RF

| USB net | All-branch centreline mm | Signal vias | All CP2102N/ESD/USB-C lands natively connected |
|---|---:|---:|---|
| /USB/USB_DP | 17.808231 | 2 | True |
| /USB/USB_DM | 9.869068 | 3 | True |

These lengths include connector orientation ties and ESD branches, not endpoint-to-endpoint pair skew. CP2102N uses 12Mbit/s full-speed USB. Most traces are 0.214355–0.214400mm; the moved-via dogbones include 0.15mm segments. Layer changes and local dogbones do not constitute a uniform controlled-impedance differential pair. In1 projection checks include normal antipads and do not establish the adjacent reference of B.Cu/In2. Enumeration/data-transfer/ESD and eye testing remain unmeasured.

LTE/GNSS use 0.29mm F.Cu traces over the In1 GND plane; nominal coplanar gap0.25mm, dielectric0.2104mm, JLC04081H-7628. The retained 2-D estimate is approximately50.11Ω, not fabricated impedance. Local GND transfers were added at both U.FL connectors and modem RF launches; new via copper was screened at least0.58mm from RF copper. Antenna match, OTA/desense and enclosure coexistence require assembled tests.

## Via-in-pad disposition

Native drill/SMT-land intersections: **54 vias**, including **9 centred**; **43 annuli extend partly outside the associated land copper. Total board vias: 358. Unfilled or uncapped retained in-pad holes: 0. See `via-in-pad-map.csv` and `manufacturing-geometry.json` for exact drill, diameter, location and affected lands. Historical centred-only count28 was not a complete census.

Unnecessary overlaps were trial-moved to off-pad dogbones with actual DRC rollback. Some remain in dense/fine-pitch escapes and high-current pads. Do NOT substitute ordinary tenting/ink plugging for resin fill + planarization + copper cap. JLC supports POFV, but four-layer POFV is chargeable; no numeric quote or DFM acceptance of these partial overlaps/0.15mm drills has been obtained. The 2026 guide and older process notice differ on preferred drill range; supplier confirmation of actual files is mandatory.

## Assembly and mechanical gates

BOM/CPL contain **123 fitted positions**, **55 active LCSC codes**, top side only. Unverified populated manufacturers: 0; missing MPNs: 0; current assembly classification anomalies: []. Supplier identity is not stock allocation. R19 uses verified SMT Vishay C2117579; C39/C68 Kyocera AVX identity was confirmed by LCSC.

Standard PCBA is required for ESP32/EG912U; EG912U needs X-ray. The82×38.5mm unit needs JLC-designed panel/fixture/rails/fiducials, allowing ESP antenna overhang and USB/SD openings. No panel file was generated. BOM/CPL designator sets were compared; final supplier part-match, rotations, pin1 and polarity preview remain mandatory. Tantalum anodes are pad1 on MODEM_3V8, cathodes pad2 on GND. Battery connector U2 pad1=VBAT, pad2=GND; confirm purchased mating harness polarity before connection.

Manual integration: attach the specified enclosure-mounted LTE/GNSS antennas/coax, route/retain excess cable without crossing the ESP antenna or pinching the pouch, assemble the insulated battery/harness, and install the mechanical supports. Use professional paste/reflow/X-ray for no-lead parts, not assumed iron access. Bare TP9/TP10 are not purchased components; DNP RF shunts remain unpopulated unless tuning measurements specify otherwise.

Mechanical planning envelope84.6×54×16mm remains conditional on actual maximum cell dimensions and swelling; nominal underside gap1.75mm includes0.25mm continuous insulation and an unqualified1mm reserve. No bottom-side parts; through-hole solder must not press on the cell. ESP32 15mm external clearance is not met; user-approved LTE overlap is not RF isolation. GNSS RF1/U.FL cable mating and firmware CFUN4/QGPS behavior still need hardware/firmware acceptance.

## Regeneration

From the repository root, with system KiCad/pcbnew, NumPy, SciPy, Shapely and the retained KiCadRoutingTools checkout installed:

```sh
/usr/bin/python3 agent/companion/scripts/verify.py
/usr/bin/python3 agent/companion/scripts/assembly.py
/usr/bin/python3 agent/companion/scripts/write_handoff.py
/usr/bin/python3 agent/companion/scripts/release_revision.py
```

The exporter writes only the engineering candidate. Do not copy it to `manufacturing/companion/rev-b/` until supplier DFM/POFV, stock/BOM/CPL review and engineering acceptance are completed. See `reports/jlcpcb-dfm/before-after.md`, `reports/jlcpcb-dfm/approval-exceptions.md`, `reports/jlcpcb-dfm/inventory.json`, `research/manufacturing.md`, `research/mechanical-revision.md`, `reports/sourcing-recheck.md`, and the candidate verification/manifest files for sources and limitations.

## Rev B DFM remediation and supplier gates

The supplied JLC reports are preserved and deduplicated in `jlcpcb-dfm/inventory.json`. Exact before/after measurements, source limitations and category dispositions are in `jlcpcb-dfm/before-after.md`; pair-by-pair unapproved spacing, edge/fixture and process exceptions are in `jlcpcb-dfm/approval-exceptions.md`.
Manufacturer copper unions and separate stencil apertures correct the FET, charger and converter land patterns without changing circuit nets or component poses. `jlcpcb-dfm/pattern-verification.json` independently checks the emitted Gerber flashes/regions, all corrected lands and apertures, mask coverage and retained filled/capped vias. This does not certify terminal tolerances, reflow or supplier stencil registration. TI examples use a 0.125mm charger stencil and 0.1mm converter stencil; JLC must approve the common or stepped process.
R19 is locally identified as Vishay MCS04020C5492FE000 / C2117579, with manufacturer and native geometry evidence. The supplied JLC result still lacks its assembly package assignment. No supplier-side correction was observed or submitted; `r19-package.json` and `r19-supplier-request.txt` define exact closeout. Do not treat a locally valid BOM/CPL as a cleared supplier package.
The compact modem layout retains sub-3mm manufacturer-recommended soldering/maintenance gaps. They remain unapproved assembly exceptions, not false positives. The full exception table and matching Standard-PCBA fixture/rail relief must be explicitly accepted by JLC before ordering.

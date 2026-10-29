# Manufacturer evidence and working assumptions

Retrieved 2026-10-28. Original project remains in ../../kicad; full snapshot in ../backups/source-kicad.

## JLCPCB

Primary source: https://jlcpcb.com/impedance . Live page required selecting 0.8mm and waiting for dynamic content; static default is 1.6mm and must NOT be used. Raw selected page text: jlc-stackup-live.txt.

Selected standard stackup **JLC04081H-7628**, outer copper 1oz, inner copper 0.5oz:

| Item | Thickness mm | Material Dk |
|---|---:|---:|
| F.Cu | 0.035 | copper |
| Dielectric 1 | 0.21040 | 7628, 4.4 |
| In1.Cu | 0.0152 | copper |
| Core | 0.250 | FR4, 4.6 |
| In2.Cu | 0.0152 | copper |
| Dielectric 3 | 0.21040 | 7628, 4.4 |
| B.Cu | 0.035 | copper |

Metal plus dielectric sum = 0.7712mm. Nominal 0.8mm finished board; do not infer tight finished thickness tolerance from this arithmetic. JLC mask model above trace is 0.6mil = 0.01524mm, Dk 3.8; above substrate/between traces 1.2mil. KiCad uniform masks approximate this contoured coating. Copper geometry alone does not establish fabricated impedance.

RF calculation: scripts/rf_impedance.py solves 2D quasi-static capacitance with 5um cells, 0.2104mm reference-plane spacing, 0.035mm copper, 0.25mm coplanar gap and approximate mask. At width 0.29mm it predicts 50.1113 ohms. Finite boundaries, dielectric tolerance, mask shape, etching, pad launches and actual assembly require fab impedance review and RF validation. Not a measured impedance claim.

Assembly source: https://jlcpcb.com/capabilities/pcb-assembly-capabilities ; raw dynamic table saved in jlc-assembly-live.txt. Economic: single side only, standard stackups only, 0.8–1.6mm, single PCB minimum10x10mm, minimum0402 / IC pitch0.4mm. Standard: one or both sides, minimum70x70mm, rails and fiducials necessary. The revised **82x38.5mm all-top design needs Standard PCBA panel/fixture**, because ESP32 and EG912U are catalogued Standard Only and the unit is narrower than70mm. **User selected JLCPCB-supplied panelization; no panel PCB or panel Gerbers will be generated.** Supply unit-board files and request supplier-designed rails, tabs, fiducials and routed cavities. Clearance must accommodate6.53mm ESP antenna overhang; no rail/tab under the antenna or across SD/USB openings. Supplier DFM approval remains required. Unit dimensions stay82x38.5. No purchase/order submitted.

Catalog verification: all55 distinct populated LCSC codes have live JLCPCB identity records saved in catalog.json. ESP32 C5361945 and EG912U C7498995 are explicitly **Standard Only**; EG912U requires X-ray inspection. Source https://jlcpcb.com/partdetail/C5361945 and https://jlcpcb.com/partdetail/C7498995 . Stock/order-allocation was not verified. R19 C54530922 is inconsistently tagged Wave Soldering despite being0402; require supplier correction/confirmation or manual population, not an assumed SMT match.

## Espressif

https://documentation.espressif.com/esp32-wroom-32e_esp32-wroom-32ue_datasheet_en.pdf v2.1 confirms ESP32-WROOM-32E-N8R2, 8MB flash +2MB PSRAM, Bluetooth4.2 BR/EDR +LE, module18.0x25.5x3.1mm. Figures10/13 give dimensions and6.19mm antenna region. Existing footprint has antenna along local-X, NOT local-Y. A270degree rotation points antenna toward board-Y.

https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/pcb-layout-design.html recommends antenna beyond baseboard, feed point close to edge, continuous reference ground under electronics, and at least15mm antenna/housing clearance. The revised carrier supports all pads and keeps copper/vias/pads/pours outside the actual overhanging antenna rectangle. Ground behind the antenna feed, beneath shielded electronics, remains referenced.

**Revised user-approved packaging compromise:** the old global16–18mm battery gap is superseded, not a requirement. All components now face outward; battery projection is shifted8.25mm away from the antenna edge, with1.75mm nominal underside gap including0.25mm insulation and an unqualified1mm swelling/tolerance reserve. The16mm-thick enclosure places the selected LTE and GNSS antennas on opposite internal faces. Espressif's15mm external clearance is NOT met locally; ESP32/LTE projection overlap and close battery/coax remain documented RF qualification risks. See mechanical-revision.md and measured mechanical-revision.json. Plastic faces provide no RF shielding.

## Quectel

Local datasheet Quectel_EG912U-GL_Hardware_Design_V1.1.pdf, 2023-08-17. Extracted searchable text quectel.txt. Pin49ANT_GNSS and60ANT_MAIN are50ohm. Quectel staff explicitly confirms **EG912UGLAA-I05-SGNSA supports GNSS**, Bluetooth and WiFi scan at https://forums.quectel.com/t/what-is-full-part-number-for-eg912u-gl-with-optional-wifi-scan/49052 (reply2025-08-19). Existing GNSS path has100pF series DC block and DNP pi shunts, suitable for a passive antenna without active-antenna bias supply. Do not attach an active GNSS antenna requiring bias without a documented design correction.

Sections3.5.3/5.3: VBAT3.3–4.3V, low-ESR100uF plus100nF/33pF/10pF at both supply branches; recommended BB trace>=2mm, RF>=2.5mm. Continuous ground reference; no thermal spokes on RF ground pins; RF50ohm, short,135degree bends, no reference discontinuity; via distance from RF>=2W. GNSS/external LTE antenna isolation>40dB is an antenna/enclosure measurement requirement, not something DRC establishes.

## Connector drawings

XUNPU TF-115 drawing: socket14.75mm wide,14.5mm deep,1.8–2.0mm high; pin tails opposite card entry; card11x15mm; push-in/out travel shown to5.5mm from socket face. Existing footprint includes inserted card protrusion. Reserve card extraction/finger access outside its actual opening. HRO USB-C default opens+Y. Molex2053380002 drawing confirms side-entry2.6mm-high connector, mating assembly11.35mm long, housing205341**** and gold contacts2053425028 (AWG20–22) or2053425128 (AWG24–26). Matched battery harness and verified polarity are additional assembly items, not automatically supplied by battery dimensions. Nail3's90degree local rotation exchanges its2.9x1.4 axes; this is NOT a footprint-width error.

3D model calibration found real origin errors in source library: ESP32 model needs local-X offset-7.557008mm to align its25.5mm body and asymmetric antenna/shield with the solder pads; USB model needs local-Y physical offset-2.24mm (CAD+Y2.24), verified against asymmetric locating pegs and gold signal feet; battery model needs local-Y physical offset-1.055mm to center its two contact feet on pads. Mechanical.py records native solid bounds. The modem originally had no model: added a clearly labelled29x25x2.4mm rectangular mechanical envelope, not a detailed vendor solid. Artwork moved from silk onto fabrication layers is preserved in project-local footprint libraries without changing land patterns or schematic IDs.

## ERC caveat

Original project disabled pin_to_pin entirely. Working copy restores it; custom imported symbols have many Unspecified pins, generating warnings. Do not confuse baseline three warnings under suppressed conflict checks with a fully typed electrical audit. No safety rules weakened to force a pass.

# Independent electrical review — companion

Read-only, one pass, 2026-10-29. Termination criterion: inspect the assigned saved native PCB, schematic hierarchy, named calculation scripts, manufacturing/mechanical research and relevant manufacturer datasheets once; deliver ranked measured findings and smallest repairs, without reopening the review after repairs. No canonical PCB, schematic, or script was modified. No DRC/ERC, fabrication export, test, or qualification was run.

## Snapshot and verdict

Reviewed `kicad/companion/companion.kicad_pcb`, SHA-256 `7dec67a7b60a15d2b79b1198c54ab1a1da60eb94bd500bdc4305c17eaad91bd6`: **129 footprints, 600 pads, 2,002 track segments, 334 vias, 24 zones**. Parent continued routing concurrently; all measurements below belong to this in-memory snapshot, not a declaration about the eventual saved board.

**FIX FIRST: E1 local U4 output bypass; E2 local U5 boost output bypass; E3 RF launch ground transfers; E4 charger switching-current neck.** These are layout risks with measured geometry and manufacturer guidance, not observed hardware failures. Parent has accepted local capacitor moves and is implementing repairs; their resulting copper, clearance and performance are outside this single-pass review.

The eight already-known opens are explicitly excluded as new findings: SW1 split zone, COMP, DTR, UART_ACTIVE, two SYS_3V3 branches and two USB_5V branches. The priority-3 SW1 B.Cu zone being cut by priority-21 VSYS is part of the parent's existing repair, not E1–E4.

## Ranked findings

### E1 — SERIOUS: U4 has no local output capacitor

U4 TPS63802 output pin 6 is at **(76.000, 62.180)**. The nearest connected bypass of any value is C58 100 nF, **8.7817 mm pad-centre distance / 7.9826 mm native copper-edge gap**. The closest 22 µF output capacitor is C10, pad 1 at (77.775, 74.250), **12.1998 mm centre distance / 10.7985 mm copper gap**. C11 22 µF is 25.3446 mm away; C57 10 µF is 15.1761 mm away. There is no unnoticed small MLCC adjacent to pin 6.

TI TPS63802 §12.1 requires the input/output capacitors close to the IC with wide, short, direct connections. Its output-capacitor selection matrix does not mark 10 µF as a supported standalone output capacitance for 0.47 µH. The nearest 100 nF does not replace the required bulk output capacitance. Failure scenario: excess switching/output-loop inductance and poorer transient response during the assumed **0.6 A ESP32** load; stability/overshoot magnitude is not established by this geometric review.

**Smallest screened repair:** move existing C10 to **(77.90, 66.95), 0°**, move existing C25 to C10's old position **(79.25, 74.25), 0°**. C10 pad 1 becomes (76.425, 66.950), GND pad 2 (79.375, 66.950), reducing native output-pad/cap-pad gap to **3.4200 mm**. Route the output directly on F.Cu with the shortest practical connection and give C10 ground a short, local return to the regulator ground/plane. This is an improvement constrained by the unchanged major-component poses, not an assertion that 3.42 mm is an ideal reference layout.

**Input distinction:** C9 is the existing 10 µF VSYS bypass: pad 2 (74.750, 65.475), **2.4204 mm native copper gap** to U4 power VIN pin 10. Leave this nearer input capacitor in place rather than displacing it for output copper. **C8 is VBAT/GND at the charger, not a U4 input bypass.** C25 relocation does not remove all local modem-input capacitance: C6 and C26 remain on VSYS, but parent must preserve their native load-path connections.

### E2 — SERIOUS: U5 boost hot-loop output capacitors are remote

The nearest 22 µF capacitor C33 has output pad (96.750, 67.500), **7.7648 mm from U5 pin 12**, with **5.8164 mm minimum gap from the union of all native U5 output lands**. Nearest 100 nF C38 is 6.9378 mm from pin 12 / **5.4101 mm from the output-land union**. C31/C32/C34 and the tantalum capacitors do not provide an adjacent output bypass.

TI TPS552882 §10.1 explicitly identifies the boost-side FET → rectifier → output capacitor → ground path as the critical nanosecond-edge current loop, and requires the capacitor near both VOUT and PGND to reduce SW2/VOUT overshoot. At the assumed **3 A modem output** this is a power switching loop, unlike the thin FB/COMP control branches. Failure scenario: additional boost-loop inductance and switching overshoot/noise. No overshoot voltage, stability margin or fabricated thermal result was measured.

**Smallest screened repair:** move existing C33 to **(88.55, 68.40), 270°**, and move only its immediate obstructing small compensation capacitor C30 locally to **(90.30, 68.40), 0°**. C33 output pad 1 becomes **(88.550, 67.400)** and GND pad 2 **(88.550, 69.400)**. Native output-land/cap-land gap becomes **0.2125 mm**; this is an edge-to-edge gap, not a route-length estimate. Route output directly to the U5 south output lands; give cap GND a local via/short return to U5 PGND rather than relying on a remote ground attachment. C30 remains in the local compensation area rather than being moved to C33's old distant site. Parent separately mentioned a possible C29 rotation; that additional pose was not screened by this reviewer.

### E3 — SERIOUS: missing local RF launch ground transfer/stitching

Both RF traces have continuous saved In1.Cu GND directly underneath their entire native trace-width geometry; **this is not a missing-plane-under-the-signal finding**. However, coplanar top ground and U.FL ground lands have no nearby transfer to that reference plane:

| Location | Closest GND via centre, measured from native ground-pad copper |
|---|---:|
| J_GNSS1 pad 1 | **20.2097 mm** |
| J_GNSS1 pad 3 | **17.5719 mm** |
| J_LTE1 pad 1 | **3.9098 mm** |
| J_LTE1 pad 3 | **3.5722 mm** |

There are **46 GND vias** in the reviewed snapshot. The only GND plated-through footprint pads are the four USB shell pads, not hidden RF-launch ground transfers. U10 ANT_GNSS pin 49 (107.290, 58.100) is **14.4841 mm centre-to-centre** from its nearest GND via; ANT_MAIN pin 60 (105.900, 72.380) is **5.9051 mm** away. J_GNSS/J_LTE ground lands are on top copper, not through-hole lands.

The actual RF routes are 0.29 mm F.Cu over In1 GND, with a minimum native coplanar gap about **0.2505 mm**. The 2-D GCPW model assumes coplanar ground at a defined ground potential; absent local stitching, this assumption is not physically secured at the launches. Failure scenario: additional launch return inductance and coplanar/plane mode conversion; actual insertion loss, match or receiver desense is unmeasured.

**Smallest repair:** short GND dogbones/stitches at both ground pads of each U.FL and adjacent modem RF ground lands, joining F.Cu to In1 GND. Retain the signal/reference geometry and keep vias at least **2W = 0.58 mm** from RF traces in accordance with the local Quectel guidance. Parent has explicitly taken this repair. No antenna/enclosure placement change is requested by this finding.

### E4 — SERIOUS: charger SW load path is still signal-width copper

BQ25606 SW pins 19/20 to L3 use only **0.15 mm** track width, through a two-via F.Cu → B.Cu → F.Cu series path. This is `Net-(C4-Pad1)` (C4 is the bootstrap capacitor); it is not a low-current control net. Segments:

| UUID | Layer | Start → end mm | Centreline length mm |
|---|---|---|---:|
| `edea96ca-2961-4603-b1c2-76a7de581ecb` | F.Cu | (76.200,72.250) → (74.750,73.700) | 2.050610 |
| `879d3d47-79cb-4c5c-b5e4-e67dc96c9e45` | B.Cu | (75.550,71.600) → (76.200,72.250) | 0.919239 |
| `a8b68e74-cda3-4f1b-88cd-eedf39058530` | F.Cu | (74.700,71.700) → (75.150,71.700) | 0.450000 |
| `723b87c3-a66f-4142-a523-4b92be7fcec9` | F.Cu | (74.750,73.950) → (75.250,73.950) | 0.500000 |

After excluding native same-net pads and via annular disks, the main diagonal has **1.5288 mm bare 0.15 mm F.Cu**, and B.Cu has **0.5668 mm bare 0.15 mm**. At nominal 35 µm outer copper these two bare lengths alone give approximately **6.88 mΩ** using copper resistivity 1.724×10⁻⁵ Ω·mm, excluding spreads, overlaps and vias. This is an ohmic estimate, not a temperature-rise prediction. The two vias are **0.60/0.30 mm**; assuming 20 µm barrel plating and 0.756 mm barrel length gives about **0.691 mΩ per via**, but plating was not measured.

**Important load distinction:** battery-only VBAT → internal BATFET → VSYS current bypasses this SW/inductor branch. Do not apply the entire assumed 4 A battery current to SW. SW carries the charger buck/OTG inductor current; with a 2 A USB input its output/inductor current can still exceed 2 A depending on voltages/efficiency. Thin copper here is relevant for both DC loss and switching parasitics, not proof of a 4 A fuse link.

TI BQ25606 §12.1 requires SW-to-inductor copper short and wide enough for charging current, with minimum loop area and no parallel multi-layer SW arrangement. **Smallest repair:** replace the detour with a short, locally widened F.Cu SW connection from the paired pads to L3 where practical, flaring after the fine-pitch pad exits; do not blindly widen across adjacent pad pitch. Parent has taken this repair. If a series via transition must remain, minimize it and screen the actual remaining neck rather than reporting nominal width alone.

**Associated input hot-loop branch:** C2 10 µF on PMID is connected by **2.6435 mm of 0.15 mm F.Cu**, of which approximately **1.6650 mm** is outside same-net pads. Its repair is a short/wide local C2–PMID connection and low-inductance ground return, not treating PMID as an ordinary control branch. UUIDs: `0f3381f2-e1c9-47f2-8a12-a26d95e0e0d6`, `1c343750-7b23-4262-a9ba-6a9d177d5835`, `6e763903-a80f-4c00-90dc-06a3ad2dde13`, `f835f8d1-4a58-492f-add7-fdaf293aacd3`.

## Candidate placement screen — exact scope

The four poses above were evaluated together, without mutating any footprint or writing a scratch board. Native shapes were transformed mathematically using KiCad's clockwise board-coordinate angle convention, calibrated against U4's asymmetric rotated lands and the custom USB VBUS land. Obstacles were the union of the native F.Courtyard, native F.Cu pad/copper bounding hull, and explicit rectangular F.Fab body regions. All existing nonmoved footprints were retained as obstacles, including U7/U10 module envelopes. Minimum mutual separation was **0.10 mm** beyond those envelopes.

| Moved part | Smallest resulting envelope gap | Neighbour |
|---|---:|---|
| C10 | **0.1300 mm** | C9 |
| C25 | **0.3800 mm** | C57 |
| C33 | **0.1463 mm** | U5 |
| C30 | **0.1200 mm** | R23 |

C33's imported C0805 courtyard is smaller than its land extent, so courtyard-only screening was not used. Its model explicitly names a nominal **2.0×1.3 mm** 0805 package; the native pad hull is larger and was retained. Imported F.Fab lead-like line/arc artwork extends roughly 3.9 mm, inconsistent with that nominal package, and was not mistaken for an actual 3.9 mm capacitor body. This is a native 2-D placement screen, **not DRC, a 3-D interference audit, a populated-board tolerance study, or clearance verification against tracks/vias**. Every moved part's existing copper must be removed/rerouted by the parent; final routing clearance and local plane access remain its verification responsibility.

## USB full-speed geometry — measured, not a new FIX FIRST finding

The fitted bridge is **CP2102N-A02-GQFN24**, whose datasheet specifies **full-speed 12 Mbit/s**, with output rise/fall time **4–20 ns**. Do not apply 480 Mbit/s modem USB acceptance criteria to these nets; the modem USB interface is not the measured bridge path.

| Net | Total native segment centreline mm | F.Cu mm | In2.Cu mm | B.Cu mm | Vias |
|---|---:|---:|---:|---:|---:|
| USB_DP | **14.608575** | 12.346138 | 0 | 2.262437 | **2** |
| USB_DM | **9.869068** | 2.051301 | 6.716726 | 1.101041 | **3** |

All signal vias here are **0.30 mm land / 0.15 mm drill**, not the 0.60/0.30 mm power vias. Track widths are **0.214355–0.214400 mm**. Totals include connector orientation ties and ESD-pad branches, and are **not endpoint-to-endpoint differential-pair lengths**. Their 4.7395 mm difference must not be reported as measured pair skew. Even using that whole difference as a conservative geometric propagation illustration gives only tens of picoseconds on FR4, not a nanosecond-scale timing assertion; no eye/compliance claim follows.

The routing is not a uniform controlled differential pair: DP is principally F.Cu while DM's main route is In2.Cu, with unequal transitions and bottom connector ties. The adjacent reference of B.Cu is In2 (power copper), not In1 GND; In1 is approximately **0.4756 mm dielectric/copper spacing away from B.Cu**, whereas F.Cu directly faces In1 across **0.2104 mm**. In2-to-In1 spacing is the **0.250 mm core**. Some trace-width projections also intersect normal GND-plane via antipads. This is nonuniform geometry, but this review does not establish a full-speed timing/eye failure on these short routes. **No additional mandatory reroute is claimed solely from these lengths or layer differences.**

`audit_usb_resume.py` has material scope limitations: its graph unions lines without layer identity, omits via electrical path length, uses pad bounding boxes with 0.12 mm inflation, and checks every layer against the projected In1 plane. Thus its reported graph paths/reference booleans cannot prove physical layer-aware connectivity or uniform impedance. Parent's final native connectivity/clearance verification and subsequent USB operation testing remain necessary.

## Power-path interpretation and checked-correct observations

Assumed review loads: **VBAT 4 A; VSYS 4.1 A; modem 3 A; USB 2 A; ESP32 0.6 A.** These are screening assumptions, not measured operation or qualified ratings.

- Native stackup matches the research dielectric/copper values: outer 35 µm, In1/In2 15.2 µm, outer dielectric 0.2104 mm, core 0.250 mm. The physical thickness record is nominal 0.8 mm; material sums/tolerances do not establish fabricated impedance.
- Main VSYS load enters QH2's large drain land, with **three 0.60/0.30 mm vias at (81.8,64.94), (82.5,64.94), (83.2,64.94)**. The thin VSYS route from U5 VIN pin 3 at (86.16,64.00) is an IC bias/control branch, **not the 4.1 A QH2 power feed**. U4 power VIN pin 10 is distinct from its small VIN/EN-side pin 1 route.
- Modem supply pins are four native lands: U10.32/.33 at (126.59,58.10)/(125.49,58.10), and .52/.53 at (105.90,62.18)/(105.90,63.28). Each has a nearby 0.60/0.30 mm via; supply copper includes a **1,460.229 mm² saved In2 zone** and **133.931 mm² saved B.Cu zone**. A universal '0.15 mm minimum on this net' alarm would misclassify RF-bypass tails, divider/feedback sensing and existing broad output copper. No additional whole-rail current neck is demonstrated by that minimum alone.
- VBAT's thin 0.15 mm tails include the fuel-gauge branch and the C8 bypass tail. Those are not substitutes for the U2–U1 load route. Similarly, USB VAC sensing and bridge supply do not each carry the charger input load. Known USB branch opens remain parent's existing work, not new evidence of an intact rail.
- `power_mesh_resume.py` correctly limits itself to a 0.1 mm DC screen with assumed 20 µm barrel plating and separate load terminals. It does **not include the SW/PMID hot-loop cases or capacitor ESL**, cannot resolve all sub-grid necks, and does not qualify copper temperature, transient response, current limits or manufacturability. No fresh solver run was performed, and its historical voltage-drop values are not adopted as post-repair measurements.
- Complete LTE/GNSS copper chain legs were independently measured, not only the ANT-labelled legs. All are **0.29 mm F.Cu, zero signal vias**, and have **zero native trace-copper area outside the saved In1 GND polygon**. LTE legs: 5.859949 mm (`LTE_ANT`) and 5.065584 mm (connector-side). GNSS legs: 9.021017 mm (`GNSS_ANT`), 7.588427 mm (intermediate C50 net), 4.661599 mm (connector/DC-block side). Sums include matching-network branches and are not electrical reference-plane lengths.
- `rf_impedance.py` is explicitly a quasi-static 2-D estimate, not a fabricated impedance result. Its assumed 0.25 mm coplanar gap agrees closely with measured ~0.2505 mm, but local stitching, launches, actual dielectric/mask/etch tolerances and populated enclosure are outside that solver.

## Via-in-pad and already documented risks

No new VIP process finding is added: the parent explicitly owns the final native polygon/hole audit and filled-and-capped process disposition. Parent reports **124 hole/actual-SMT-polygon intersections, 24 centred**, with all retained in-pad holes requiring filled/capped treatment; these are **parent-provided current numbers, not independently reproduced here**. The historical manufacturing report's **28 centred-only** count was an undercount of actual via/pad intersections and must not be reused as a complete VIP census. Tenting alone is not filled/capped construction. Parent's final manufacturing evidence must reconcile retained holes and supplier process approval; this review does not claim that fabrication has accepted the process.

The mechanical research already records the noncompliant local ESP32 antenna clearance, permitted ESP32/LTE overlap, close battery/coax, and the requirement for assembled-device RF and thermal testing. Those are not newly discovered geometry findings. No claim is made that plastic separates RF fields, that GNSS/LTE isolation exceeds 40 dB, or that the proposed device is thermally/battery qualified.

## Remaining acceptance concerns — do not confuse with observed failures

1. Parent must complete E1–E4 repairs and the eight pre-existing opens, preserving major component poses, then perform its single final native connectivity/clearance/placement verification. This review deliberately does not re-review or certify those edits.
2. E1's practical 3.42 mm edge gap is a substantial improvement but not a vendor-reference-layout replica; actual local output/ground copper, capacitor DC-bias derating, regulator transient response and loaded temperatures still need engineering acceptance.
3. USB full-speed operation, RF launch match/OTA performance, regulator overshoot/stability and assembled enclosure coexistence have not been measured. These are qualification limits, not newly invented reasons to move the major modules.
4. Filled/capped via-in-pad process and supplier interpretation remain order-time evidence requirements under the parent's existing manufacturing work, not a new routing blocker from this review.

## Sources

- Canonical PCB and `companion.kicad_sch` hierarchy, especially Power / ESP32 / USB / Modem sheets; native net/pad identities above.
- `datasheets/TI_BQ25606.pdf`, §12.1, especially SW, PMID and short-current-loop recommendations; [TI primary document](https://www.ti.com/lit/ds/symlink/bq25606.pdf).
- `datasheets/TI_TPS63802.pdf`, output-capacitor selection matrix / §12.1; [TI primary document](https://www.ti.com/lit/ds/symlink/tps63802.pdf).
- `datasheets/TI_TPS552882.pdf`, §10.1 critical buck/boost current paths; [TI primary document](https://www.ti.com/lit/ds/symlink/tps552882.pdf).
- `datasheets/Silicon_Labs_CP2102N.pdf`, full-speed capability and USB output rise/fall limits; [Silicon Labs primary document](https://www.silabs.com/documents/public/data-sheets/cp2102n-datasheet.pdf).
- `datasheets/Quectel_EG912U-GL_Hardware_Design_V1.1.pdf` and local searchable `research/quectel.txt`, supply branching and RF-reference recommendations.
- Assigned `scripts/power_mesh_resume.py`, `scripts/audit_usb_resume.py`, `scripts/rf_impedance.py`, `research/manufacturing.md`, `research/mechanical-revision.md`; existing manufacturing audit used only to avoid duplicating an already handled VIP/process issue.

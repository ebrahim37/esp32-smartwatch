# Revised enclosure and antenna engineering envelope

Replaces the old 82×30 mm / 16–18 mm global battery-gap analysis. Dimensions are millimetres. Configuration: `../scripts/layout-revision.json`. Nominal carrier **82×38.5×0.8**, R0.5 corners. The 0.65 mm width reduction from 39.15 allocates assembly clearance; it does not enlarge the device.

## Actual parts and tolerance arithmetic

Official drawings downloaded as `Quectel_YPCA004AA_V2.5.pdf` and `Quectel_YFGA003AA_V3.2.pdf`, corresponding drawing PNGs alongside them.

* LTE YPCA004AA: 40±0.2 ×10±0.2 ×1.15±0.1, including adhesive but excluding release paper; Ø1.13 coax 100±2, IPEX MHF I inward-facing plug. Adhesive 3M9448A. Ground-plane-independent mounting inside plastic, not permission to contact metal.
* GNSS YFGA003AA: 39.45±0.15 ×13.25±0.15 ×0.13 including tape; Ø1.13 coax 100±3, drawing calls plug **RF1**. U.FL/MHF-I mechanical compatibility must be confirmed against purchased cable assembly; RF1 alone is not a connector-series guarantee. Datasheet measurements use a 3 mm ABS mounting board, not the proposed thin printed wall.
* LTE and GNSS body projections occupy X0.5–40.5 and X42–81.45 respectively; worst length tolerances still leave ≥1.15 separation. They are on rear and front internal enclosure faces, respectively, maximizing Z separation. Their cables are not part of these body projections and require local clips/strain relief.
* At54 mm external width and0.8 mm walls, internal width is52.4. Carrier38.5 + maximum GNSS13.4 =51.9, leaving0.5 total assembly/tolerance allowance. Carrier origin is13.65 mm from the inside north wall, leaving0.25 mm at the south edge. Nominal PCB-to-FPC gap0.15, with GNSS width growth allocated toward the outer wall; nominal outer FPC-to-wall gap0.25. These are tight assembly allowances, not a fully toleranced production housing: printed accuracy, adhesive placement and board-edge tolerance require qualification.
* External length83.6 leaves82 internal, exactly the nominal cell/PCB length: no safe assembly tolerance. Use **84.6 mm nominal minimum planning length** (83 internal), conditional on cell maximum length≤82.5 and controlled printed dimensions. Supplier cell tolerance is NOT available; verify it before enclosure freeze. Greater cell tolerance increases required length directly.

## Local ESP32 clearance, not global battery separation

U7 center(13,10), rotation270°, relative to carrier northwest corner. Verified footprint module antenna lies toward negativeY. Module body overhang≈6.53; tolerance-expanded antenna rectangle X3.85–22.15,Y−6.68–−0.04. All solder lands remain supported; nearest pad copper is≈0.525 inside the carrier. No copper, vias, pads or pours may occupy the antenna rectangle on any layer. Ground beneath the shielded electronics remains supported and referenced.

Battery projection X0–82,Y8.25–38.25: an8.25 mm strip under the antenna-side carrier moves the battery away locally, while0.25 mm remaining carrier width allows assumed cell width growth. No component is fitted on the battery-facing side. LTE overlaps the ESP32 antenna projection, as explicitly permitted by the user, on the rear enclosure face. GNSS starts atX42 and does not overlap the ESP32 antenna. Route coax through the side strip atX>23 where possible, never across the ESP32 antenna, with broad curves and clipped excess100 mm cable. Do not sharply fold coax or clamp it under the pouch. Maximum cell width and swell remain unqualified.

**Deviation:** Espressif's recommended15 mm clearance around the antenna cannot be met by this54 mm-wide arrangement. Neither the battery offset nor opposite plastic enclosure faces establish RF isolation. ESP32/LTE passive coupling, Bluetooth desense during LTE transmit, detuning by battery/coax and installation loss require assembled-device OTA/VNA testing. No global15 mm underside gap is imposed or claimed necessary.

## Vertical stack / safety

Planning external thickness **16 mm**, not an asserted13 mm fit. Front/rear walls0.8 each; tallest currently modelled electronics USB shell≈3.25 and ESP32≈3.1 above top copper; PCB0.8; cell7.2; underside clear gap1.75 nominal including0.25 continuous nonconductive abrasion shield and1.0 unqualified swelling/tolerance reserve. Add front component headroom0.15: total14.75 nominal, with remaining thickness assigned to printed supports/tolerances. A vendor-qualified swollen cell envelope is REQUIRED; the assumed1.0 reserve is not a battery safety certification.

PCB supports and USB insertion load must terminate on enclosure ledges/clamps, never the pouch. Connector through-hole solder and mask/tented vias need a rigidly retained continuous electrical/abrasion barrier: no exposed joint may touch the battery at maximum swell or under mechanical load. No fastener point, wire splice, sharp ledge or coax connector presses on the cell. Provide a separately supported battery harness route to the south-edge Molex; verify polarity, cable gauge and strain relief. Charger/regulator heating and cell charge-current suitability require loaded thermal tests.

## Operating policy

Keep GNSS and LTE RF operating modes mutually exclusive in firmware, but do not use that policy as a substitute for RF geometry. Official EC200U/EG912U GNSS application note V1.2 confirms standalone QGPS operation without network assistance; it does **not explicitly guarantee QGPS operation in CFUN=4 for the purchased firmware**. That policy remains a hardware/firmware acceptance test: log ATI/GMR/QGPSINFO, enterCFUN4, startQGPS, obtain a fix and independently observe absence of cellular transmission. Do not claim validated flight-mode GNSS from documentation alone. Existing EG912UGLAA-I05-SGNSA is the GNSS-capable option; no receiver or GPIO changes made.

## Primary sources

https://www.quectel.com/content/uploads/2026/03/Quectel_4G_Antenna_YPCA004AA_Datasheet_V2.5.pdf

https://www.quectel.com/content/uploads/2026/03/Quectel_GNSS_Antenna_YFGA003AA_Datasheet_V3.2.pdf

https://quectel.com/content/uploads/2024/04/Quectel_EC200U_SeriesEG912U-GL_GNSS_Application_Note_V1.2.pdf

https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/pcb-layout-design.html

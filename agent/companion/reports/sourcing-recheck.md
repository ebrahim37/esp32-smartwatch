# JLC sourcing recheck

Live part-detail pages re-read 2026-10-29. These establish identities and assembly classifications, not reserved PCBA stock. JLC static pages did not expose order allocation or current numeric availability; LCSC retail availability is separate.

- [C5361945](https://jlcpcb.com/partdetail/C5361945): ESP32-WROOM-32E-N8R2, Espressif Systems, Standard Only, SMT Assembly, MSL3.
- [C7498995](https://jlcpcb.com/partdetail/C7498995): EG912UGLAA-I05-SGNSA, Quectel, Standard Only, X-ray Required.
- [C148107 JLC](https://jlcpcb.com/partdetail/C148107) still leaves manufacturer `--`, but the [LCSC primary C148107 record](https://www.lcsc.com/product-detail/C148107.html) explicitly identifies **Kyocera AVX TPSB107M010R0400**, 100uF 10V 400mΩ CASE-B. C39/C68 manufacturer metadata is now verified from that independent distributor record, not inferred from the MPN. LCSC metadata showed 18,912 retail units and a quantity-dependent “from $0.549”; neither is a JLC PCBA allocation or final board price.
- [C54530922](https://jlcpcb.com/partdetail/C54530922) remains incorrectly classified Wave Soldering. R19 was changed to verified SMT [Vishay Intertech MCS04020C5492FE000, C2117579](https://jlcpcb.com/partdetail/VishayIntertech-MCS04020C5492FE000/C2117579): same 54.9kΩ, ±1%, 0402, 100mW, 50ppm/K. Schematic and PCB sourcing fields are synchronized; resistance, footprint, nets and pin mapping did not change.

The populated BOM still needs an actual supplier BOM-matching/quotation stock check for every line. Catalog evidence retains historical records and the replacement identity; unused old R19 identity is not a purchasing instruction. Unit board needs supplier-designed Standard-PCBA panel/fixture. No panel, order, payment, allocation, filled/capped-via DFM approval, or assembly rotation acceptance has been performed.

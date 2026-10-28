# Component document sources

Files are named by manufacturer and part number. Existing release copies were deduplicated by content/name; newly downloaded files were validated as readable PDFs.

| Local file | Selected component / source |
|---|---|
| `Espressif_ESP32-PICO-MINI-02.pdf` | ESP32-PICO-MINI-02 family; retained manufacturer datasheet from final review release |
| `Quectel_EG912U-GL_Hardware_Design_V1.1.pdf` | EG912U-GL hardware design guide; https://quectel.com/content/uploads/2024/02/Quectel_EG912U-GL_Hardware_Design_V1.1.pdf |
| `TI_BQ25606.pdf` | BQ25606 charger; https://www.ti.com/lit/ds/symlink/bq25606.pdf |
| `TI_TPS63802.pdf` | TPS63802 buck-boost; https://www.ti.com/lit/ds/symlink/tps63802.pdf |
| `TI_TPS552882.pdf` | TPS552882 buck-boost controller; https://www.ti.com/lit/ds/symlink/tps552882.pdf |
| `TI_CSD16340Q3.pdf` | CSD16340Q3 MOSFET; https://www.ti.com/lit/ds/symlink/csd16340q3.pdf |
| `Analog_Devices_MAX17048_MAX17049.pdf` | MAX17048/MAX17049 manufacturer data sheet, selected-part distributor-hosted copy: https://live-final.oss-us-west-1.aliyuncs.com/702049/MAX17048GT10.pdf |
| `Silicon_Labs_CP2102N.pdf` | CP2102N manufacturer data sheet, LCSC-hosted selected-part copy: https://datasheet.lcsc.com/datasheet/pdf/11ab89ca3026e9cb7cc85ffe9557abff.pdf?productCode=C969151 |
| `TI_TXU0204.pdf` | TXU0204 translator; https://www.ti.com/lit/ds/symlink/txu0204.pdf |
| `TI_TPD4E05U06.pdf` | TPD4E05U06 ESD protection; https://www.ti.com/lit/ds/symlink/tpd4e05u06.pdf |
| `TI_TPD1E10B06.pdf` | TPD1E10B06 ESD protection; https://www.ti.com/lit/ds/symlink/tpd1e10b06.pdf |
| `TI_SN74LVC2G126.pdf` | SN74LVC2G126 buffer; https://www.ti.com/lit/ds/symlink/sn74lvc2g126.pdf |
| `Bosch_BMA400.pdf` | BMA400 accelerometer; Bosch Sensortec official data sheet |
| `ST_LIS2MDL.pdf` | LIS2MDL manufacturer data sheet, LCSC-hosted selected-part copy: https://datasheet.lcsc.com/datasheet/pdf/8a395828837005174075247e6d86b6df.pdf?productCode=C919695 |
| `TI_DRV2605L.pdf` | DRV2605L haptic driver; https://www.ti.com/lit/ds/symlink/drv2605l.pdf |
| `Molex_2053380002_drawing.pdf` | Molex 205338 series actuator connector drawing |
| `HRO_TYPE-C-31-M-12_C165948.pdf` | Selected HRO USB-C connector supplier/manufacturer drawing |
| `XYECONN_Nano_SIM_C53200251.pdf` | Selected nano-SIM holder supplier drawing |
| `XUNPU_TF-115_microSD_C266620.pdf` | Selected microSD holder supplier drawing |
| `DIANWEI_8.03A0.019200_C19188821.pdf` | Selected display FPC connector drawing |
| `Crystalfontz_CFAL368448A0-0193DW_display.pdf` | Candidate display drawing retained from mechanical review; exact 0193/0139 designation discrepancy remains to be confirmed |

## Known document gaps

- No authoritative final antenna/pigtail data sheet or complete enclosure/antenna integration specification is selected.
- The repository contains the selected U.FL footprint/model but not a retained official Hirose U.FL catalog sheet; obtain the current `U.FL-R-SMT-1(10)` drawing before procurement.
- The selected JST `SM02B-SRSS-TB` battery-board connector lacks a retained manufacturer drawing; obtain the current SH/SR-series drawing and confirm mating housing/crimp selection before procurement.
- The exact mating battery housing/crimp, cell data sheet, protection-circuit specification, and battery safety documents are not selected.
- The exact display part designation must be reconciled before procurement.
- Generic 3D models are not substitutes for package drawings or tolerance data.

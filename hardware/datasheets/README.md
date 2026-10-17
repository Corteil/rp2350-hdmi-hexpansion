# Datasheets

One PDF per distinct part in the schematic, plus the Raspberry Pi RP2350 datasheet and hardware design guide.
The PDFs are not stored in git (see `.gitignore`); fetch them with:

```
python fetch_datasheets.py            # download everything missing
python fetch_datasheets.py --dry-run  # just list what it would fetch
```

The list below is generated from the `LCSC Part` / `MPN` fields in `rp2350-hdmi-hexpansion.kicad_sch` (and the DNP flags in `bom.csv`). `parts.json` is the machine-readable copy the script reads.
Each part is tried at `https://www.lcsc.com/datasheet/<LCSC#>.pdf` first, then the manufacturer link where one is listed.

| Designators | MPN | LCSC | Value | File | Notes |
|---|---|---|---|---|---|
| C1, C2, C3, C4, C5, C6, C7, C10, C11, C12, C20, C29, C32, C33 | CL05B104KO5NNNC | C1525 | 100nF | `CL05B104KO5NNNC_C1525.pdf` |  |
| C13, C14, C15 | CL05A475MP5NRNC | C23733 | 4.7uF | `CL05A475MP5NRNC_C23733.pdf` |  |
| C16, C21, C23, C25 | CL10A106KP8NNNC | C19702 | 10uF | `CL10A106KP8NNNC_C19702.pdf` |  |
| C17, C18 | 0402CG150J500NT | C1548 | 15pF | `0402CG150J500NT_C1548.pdf` |  |
| C22, C24, C26 | CL21A226MAQNNNE | C45783 | 22uF | `CL21A226MAQNNNE_C45783.pdf` |  |
| D1 | SMF5.0A | C19077497 | SMF5.0A | `SMF5.0A_C19077497.pdf` |  |
| F1 | JK-SMD0805-020-30V | C516070 | 200mA PTC | `JK-SMD0805-020-30V_C516070.pdf` |  |
| J2 | TYPE-C 16PIN 2MD(073) | C2765186 | USB-C | `TYPE-C_16PIN_2MD_073__C2765186.pdf` |  |
| J3 | BM03B-SRSS-TB(LF)(SN) | C160389 | SWD | `BM03B-SRSS-TB_LF_SN__C160389.pdf` |  |
| J4 | SM04B-SRSS-TB(LF)(SN) | C160404 | Qwiic | `SM04B-SRSS-TB_LF_SN__C160404.pdf` |  |
| J5 | HDMI-001 19PCBTP | C138388 | HDMI-A | `HDMI-001_19PCBTP_C138388.pdf` |  |
| L1 | AOTA-B201610S3R3-101-T | C42411119 | 3.3uH | `AOTA-B201610S3R3-101-T_C42411119.pdf` |  |
| L2 | FTC252012S2R2MBCA | C5832372 | 2.2uH | `FTC252012S2R2MBCA_C5832372.pdf` |  |
| L3 | FTC252010S1R0MBCA | C5832356 | 1uH | `FTC252010S1R0MBCA_C5832356.pdf` |  |
| LED1, LED2 | KT-0603R | C2286 | STATUS | `KT-0603R_C2286.pdf` |  |
| LED3 | SK6805MICRO-J | C2909056 | SK6805 | `SK6805MICRO-J_C2909056.pdf` |  |
| R1, R36, R37, R38, R39 | 0402WGF330JTCE | C25105 | 33R | `0402WGF330JTCE_C25105.pdf` |  |
| R2, R6, R40, R51 | 0402WGF1001TCE | C11702 | 1k | `0402WGF1001TCE_C11702.pdf` |  |
| R3, R4, R7, R41 | 0402WGF1002TCE | C25744 | 10k | `0402WGF1002TCE_C25744.pdf` |  |
| R8, R9 | 0402WGF5101TCE | C25905 | 5.1k | `0402WGF5101TCE_C25905.pdf` |  |
| R10, R11 | 0402WGF270JTCE | C25100 | 27R | `0402WGF270JTCE_C25100.pdf` |  |
| R12, R16, R17, R18, R34, R35 | 0402WGF1003TCE | C25741 | 100k | `0402WGF1003TCE_C25741.pdf` |  |
| R13, R15 | 0402WGF2202TCE | C25768 | 22k | `0402WGF2202TCE_C25768.pdf` |  |
| R14 | 0402WGF3302TCE | C25779 | 33k | `0402WGF3302TCE_C25779.pdf` |  |
| R19 | 0402WGF2003TCE | C25764 | 200k | `0402WGF2003TCE_C25764.pdf` |  |
| R20 | 0402WGF2702TCE | C25771 | 27k | `0402WGF2702TCE_C25771.pdf` |  |
| R48, R49, R50 | 0402WGF4701TCE | C25900 | 4.7k | `0402WGF4701TCE_C25900.pdf` |  |
| R52 | 0402WGF3300TCE | C25104 | 330R | `0402WGF3300TCE_C25104.pdf` |  |
| SW1, SW2 | TS-1088-AR02016 | C720477 | RESET | `TS-1088-AR02016_C720477.pdf` |  |
| U1 | RP2354A | C41378174 | RP2354A | `RP2354A_C41378174.pdf` |  |
| U2 | TPS2116DRLR | C3235557 | TPS2116DRLR | `TPS2116DRLR_C3235557.pdf` |  |
| U3 | TLV62569DBVR | C141836 | TLV62569DBVR | `TLV62569DBVR_C141836.pdf` |  |
| U4 | TPS61023DRLR | C919459 | TPS61023DRLR | `TPS61023DRLR_C919459.pdf` |  |
| U6, U7 | TPD4E05U06DQAR | C138714 | TPD4E05U06 | `TPD4E05U06DQAR_C138714.pdf` |  |
| U8 | USBLC6-2SC6 | C2687116 | USBLC6-2SC6 | `USBLC6-2SC6_C2687116.pdf` |  |
| U9 | APS6404L-3SQR-SN | C5333729 | APS6404L-3SQR-SN | `APS6404L-3SQR-SN_C5333729.pdf` | DNP |
| U10 | ZD24C64A-SSGMT | C2896793 | ZD24C64A (fallback) | `ZD24C64A-SSGMT_C2896793.pdf` | DNP |
| U11 | SN74LVC1G07DBVR | C7829 | SN74LVC1G07 | `SN74LVC1G07DBVR_C7829.pdf` |  |
| Y1 | ABM8-272-T3 | C20625731 | 12MHz | `ABM8-272-T3_C20625731.pdf` |  |
| U1 | RP2350 | - | RP2350 / RP2354 datasheet (Raspberry Pi) | `RP2350_datasheet.pdf` |  |
| U1 | Hardware design with RP2350 | - | Raspberry Pi hardware design guide | `Hardware_design_with_RP2350.pdf` |  |

## Updating

If the schematic's parts change, regenerate `parts.json` from the schematic and re-run the script. Existing PDFs are kept unless you pass `--force`.

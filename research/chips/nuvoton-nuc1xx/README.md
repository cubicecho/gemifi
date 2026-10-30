# Nuvoton NUC100/NUC120 family (NuMicro, Cortex-M0)

Chip-level facts gathered while working on the Keystation Mini 32 MK3 (NUC120LC1BN). Device-specific wiring is in `research/devices/`.

## References
- NuMicro NUC120 Data Sheet V2.03: https://media.digikey.com/pdf/data%20sheets/nuvoton%20pdfs/nuc120.pdf (LQFP48 pin diagram: Figure 3-4).
- NUC100/NUC120 Technical Reference Manual V2.02: https://www.keil.com/dd/docs/datashts/nuvoton/nuc1xx/da00-numicronuc100f1.pdf
- Newer DN-series datasheet and TRM V1.04: nuvoton.com (DS_NUC100_120(DN), TRM_NUC100_120(DN)).
- BSP and device header: OpenNuvoton/NUC100BSP (Apache-2.0).
- OpenOCD: upstream `numicro` flash driver (lists NUC120LC1BN, ID 0x10012008) and `target/numicro.cfg`.
- TinyUSB: `src/portable/nuvoton/nuc120/dcd_nuc120.c` (device only).

## Memory map (NUC120LC1BN)
| Region | Address | Size | Notes |
|---|---|---|---|
| APROM | 0x0000_0000 | 32 KB | Application. Readable over SWD with `mdw` when unlocked. |
| Data flash | 0x0001_F000 | 4 KB | 512-byte pages. |
| LDROM | 0x0010_0000 | 4 KB | Boot loader area. Mapped at 0x0 while booting from it. Reads need ISP. |
| CONFIG0/1 | 0x0030_0000 / 4 | 8 B | Reads and writes need ISP. |
| SRAM | 0x2000_0000 | 4 KB | |
| PDID | 0x5000_0000 | | NUC120LC1BN = 0x10012008 |

## CONFIG0 bits (TRM V2.02)
| Bits | Field | Meaning |
|---|---|---|
| 28 | CKF | XT1 clock filter enable |
| 26:24 | CFOSC | Clock at reset: 000 = external crystal, 111 = internal 22.1184 MHz |
| 23 | CBODEN | 0 = brown-out detect on |
| 22:21 | CBOV | 11 = 4.5 V, 10 = 3.8 V, 01 = 2.7 V, 00 = 2.2 V |
| 20 | CBORST | 0 = brown-out reset on |
| 7 | CBS | 0 = boot LDROM, 1 = boot APROM |
| 1 | LOCK | 0 = locked (flash reads 0xFFFFFFFF over SWD), 1 = unlocked |
| 0 | DFEN | Data flash enable (128 KB parts only) |

An erased CONFIG0 (0xFFFFFFFF) means: boot APROM, unlocked, internal RC, no brown-out. That state is benign, which matters for recovering from an interrupted CONFIG write.

## Pins (LQFP48) worth remembering
- 29 ICE_DAT, 30 ICE_CK: dedicated SWD pins; firmware can't repurpose them.
- 46 /RESET (active low, internal pull-up). 44 XT1_OUT, 45 XT1_IN. 47 PVSS.
- 10 LDO (2.5 V core regulator output), 11 VDD, 12 VSS, 31 AVSS.
- Pin 1 is at the dot; numbering runs counter-clockwise.

## Debug and flash behaviour
- SWD DPIDR 0x0BB11477 (Cortex-M0). OpenOCD 0.12: use `-ap-num 0` with slow probes.
- Security lock: if LOCK = 0, SWD reads of flash return 0xFFFFFFFF. Only a whole-chip erase clears it.
- ISP (FMC at 0x5000_C000): ISPCON, ISPADR, ISPDAT, ISPCMD (0x00 read, 0x21 program, 0x22 page erase, 0x04 read UID, 0x0B/0x0C CID/DID), ISPTRG. Needs REGWRPROT unlocked (write 0x59, 0x16, 0x88 to 0x5000_0100) and ISPEN. LDUEN/CFGUEN allow LDROM/CONFIG updates.
- Code running in LDROM can't write LDROM; code in APROM can write LDROM only with LDUEN. TRM V2.02 says APROM can't program itself (ISPFF), and there's no APUEN bit in that TRM revision. The newer BSP has an APUEN macro, so check per part.
- Boot switch: unlock, set or clear ISPCON.BS (bit 1), then IPRSTC1.CPU_RST (0x5000_0008 bit 1).

## USB device controller
- 6 simplex endpoint slots; control uses 2 (EP0 IN and OUT), leaving 4. MIDI (2) plus CDC-ACM (3) doesn't fit.
- 512-byte USB SRAM buffer shared by all endpoints.
- 48 MHz from the PLL: 12 MHz crystal with PLLCON = 0xC22E.

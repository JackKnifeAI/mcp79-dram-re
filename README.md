# MCP79 DRAM Controller Reverse Engineering 🔬

**The first-ever public documentation of NVIDIA MCP79 (nForce 730i) memory controller registers.**

Reverse engineered from Apple EFI firmware (MacBook 5,x / iMac 9,1) by [JackKnife Studios](https://github.com/JackKnifeAI), October 2026.

## What Is This?

The NVIDIA MCP79 is the chipset used in 2008-2009 Apple MacBooks, MacBook Pros, and iMacs (the ones with the GeForce 9400M). Despite being in millions of machines, the DRAM controller registers were **never publicly documented** — not by NVIDIA, not by envytools, not by nouveau. Until now.

We extracted the complete register map by reverse engineering Apple's EFI firmware using radare2, decoding the PEI (Pre-EFI Initialization) phase memory controller init code.

## 📋 What's Documented

### Memory Controller Register Map ([docs/MCP79_MC_REGISTER_MAP.txt](docs/MCP79_MC_REGISTER_MAP.txt))

- **4 PCI devices** mapped (00:00.1, 00:03.1, 00:03.3, 00:03.4)
- **15+ standard PCI config registers** decoded with bit fields
- **11 extended config registers** (0x080-0x5AC) for aperture/region control
- **DDR3 timing parameter table** with 4 speed grades
- **MRS (Mode Register Set) command interface** — how to send commands to DDR3 chips
- **Complete 11-phase DRAM initialization sequence** (JEDEC-compliant)
- **19 firmware functions** mapped with descriptions
- **DRAM command dispatcher** — PRE, NOP, REF, MRS, ZQ calibration

### Key Registers

| Device | Register | Description |
|---|---|---|
| 00:03.4 | 0xAC | **tREFI** — Auto refresh interval (5120 clocks @ DDR3-1333) |
| 00:03.4 | 0xE8 | **MRS values** — MR0/MR1/MR2 encoded command register |
| 00:03.4 | 0x68 | **tRAS** — Row Active Strobe timing (per rank) |
| 00:03.4 | 0x6C | **tRC** — Row Cycle time |
| 00:03.4 | 0x7C | **tRTP/tWTR** — Short timing params (per byte lane) |
| 00:03.1 | 0x68 | **DIMM geometry** — banks, DIMMs, ranks |
| 00:03.1 | 0x6C | **CAS Latency** — confirmed CL9 for DDR3-1333 |
| 00:03.1 | 0xAC | **DRAM density** — die capacity and type |
| ext 0x400 | — | **MC Enable** — master memory controller enable (bit 8) |

### MRS Command Interface

The MCP79 uses a vtable-based MRS command protocol:

```
MR0 → command byte 0x50 (CAS latency, burst length, write recovery)
MR1 → command byte 0x51 (output drive strength, ODT)
MR2 → command byte 0x52 (CAS write latency, RTT_WR)
MR3 → command byte 0x53 (MPR mode, test mode)
```

Commands are dispatched through `MC_MRS_Command` (firmware offset 0xC118) and iterate over all populated DIMM ranks.

## 🧪 DRAM Spare Row Probe (Work In Progress)

We're building a Linux kernel module (`dram_probe.ko`) that uses this register map to:

1. **Map DRAM physical topology** via row conflict timing analysis
2. **Probe for hidden spare rows** via retention time scanning (using the tREFI register to temporarily disable refresh)
3. **Send MRS commands** to DDR3 DIMMs for test mode access
4. **Attempt to access unused spare rows** — DRAM dies contain ~5-10% extra physical rows for yield repair, most of which go unused

See [`src/`](src/) for the kernel module source.

## 🔧 Hardware

Tested on:
- **MacBook** with MCP79 chipset (GeForce 9400M)
- **DDR3-1333** SO-DIMMs (Nanya NT2GC64B88B0NS-CG, 2Gbit J-die)
- **Linux 6.8.0** (Mint 21.3 / Ubuntu 22.04 HWE)
- **SPD access** via I2C bus 0, addresses 0x50/0x51

Should work on any MCP79-based system:
- MacBook 5,1 (Aluminum Late 2008)
- MacBook Pro 5,1-5,5 (Late 2008 - Mid 2009)
- iMac 9,1 (Early/Mid 2009)
- Mac Mini 3,1 (Early 2009)
- Any nForce 730i / GeForce 9400M system

## 📖 How We Did It

1. Dumped 4MB SPI flash via `flashrom -p internal` → raw EFI firmware
2. Extracted 837 modules via `uefiextract`
3. Found memory init code by searching for PCI CF8 addresses (0x1C00 = 00:03.4)
4. Disassembled 32-bit PEI code with radare2
5. Traced every PCI config write to memory controller devices
6. Cross-referenced with live register values read via `setpci`
7. Validated timing values against DDR3-1333 JEDEC specifications

## ⚠️ Safety

- **Reading** MC registers is completely safe
- **Writing** MC registers can cause crashes — but ALL DRAM state is **volatile** (power cycle = full reset)
- **NEVER write to SPD EEPROMs** — that's the only persistent thing that can prevent boot
- The kernel module includes a **watchdog timer** that auto-restores all registers if something goes wrong

## 📚 References

- [envytools](https://envytools.readthedocs.io/) — NVIDIA GPU RE project (MCP79 MC was listed as TODO)
- [HiRA: Hidden Row Activation](https://arxiv.org/abs/2209.10198) — ETH Zurich, MICRO 2022
- [DRAMScope](https://arxiv.org/abs/2405.02499) — Seoul National University, ISCA 2024
- [DDR3 JEDEC Standard](https://www.jedec.org/standards-documents/docs/jesd-79-3f) — JESD79-3F

## License

MIT License. Research by JackKnife Studios. Do whatever you want with it.

---

*"Some people see old hardware and think 'ewaste.' We see it and think 'how deep does this rabbit hole go?'"*

*— JackKnife Studios, 2026*

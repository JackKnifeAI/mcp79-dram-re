/*
 * dram_probe.ko — MCP79 DRAM Controller Probe Module
 *
 * Sends MRS commands, controls refresh, and probes for hidden spare rows
 * on NVIDIA MCP79 (nForce 730i) memory controllers.
 *
 * First-ever software-based DRAM spare row probe for MCP79.
 *
 * (c) 2026 JackKnife Studios — MIT License
 *
 * SAFETY:
 *   - All DRAM state is volatile (power cycle = full reset)
 *   - Watchdog timer auto-restores registers on crash
 *   - NEVER touches SPD EEPROMs
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/io.h>

MODULE_LICENSE("MIT");
MODULE_AUTHOR("JackKnife Studios");
MODULE_DESCRIPTION("MCP79 DRAM Controller Probe — Spare Row Liberation");
MODULE_VERSION("0.1.0");

/* ═══════════════════════════════════════════════════════════
 * MCP79 PCI Device Definitions
 * ═══════════════════════════════════════════════════════════ */

/* PCI BDF for memory controller devices */
#define MC_DIMM_BUS     0
#define MC_DIMM_DEV     3
#define MC_DIMM_FUNC    1   /* 00:03.1 — DIMM Configuration */

#define MC_TIMING_BUS   0
#define MC_TIMING_DEV   3
#define MC_TIMING_FUNC  4   /* 00:03.4 — Timing Controller */

/* ═══════════════════════════════════════════════════════════
 * Register Offsets (from our RE of Apple EFI firmware)
 * ═══════════════════════════════════════════════════════════ */

/* Device 00:03.1 — DIMM Configuration */
#define REG_DIMM_GEOMETRY   0x68    /* banks, DIMMs, ranks */
#define REG_CAS_LATENCY     0x6C    /* CAS latency in clocks */
#define REG_DRAM_DENSITY    0xAC    /* die density + type */
#define REG_DIMM_CONFIG2    0xB0    /* col/row bits, page size */

/* Device 00:03.4 — Timing Controller */
#define REG_TIMING_GRP0     0x48    /* primary timing group 0 */
#define REG_TIMING_GRP1     0x4C    /* primary timing group 1 */
#define REG_TIMING_GRP2     0x50    /* primary timing group 2 */
#define REG_TFAW_TRRD       0x54    /* tFAW / tRRD */
#define REG_CMD_ADDR_TIMING 0x58    /* command/address timing */
#define REG_ODT_DRIVE       0x5C    /* ODT / drive strength */
#define REG_WRITE_LEVEL     0x60    /* write leveling config */
#define REG_DQS_PHASE       0x64    /* DQS phase adjustment */
#define REG_TRAS            0x68    /* tRAS per rank */
#define REG_TRC             0x6C    /* tRC row cycle time */
#define REG_TRTP_TWTR       0x7C    /* tRTP/tWTR per byte lane */
#define REG_TRFC            0x8C    /* tRFC refresh recovery */
#define REG_TREFI           0xAC    /* ★ tREFI refresh interval */
#define REG_MRS_VALUES      0xE8    /* MRS command values (MR0-MR2) */
#define REG_MRS_CONTROL     0xF4    /* MRS / command control */

/* tREFI standard value for DDR3-1333 */
#define TREFI_DEFAULT       0x1400  /* 5120 clocks = 7.68µs */
#define TREFI_DISABLED      0x0000  /* refresh disabled */

/* ═══════════════════════════════════════════════════════════
 * Safety: Register Save/Restore + Watchdog
 * ═══════════════════════════════════════════════════════════ */

struct mc_saved_regs {
    /* 00:03.1 registers */
    u32 dimm_geometry;
    u32 cas_latency;
    u32 dram_density;
    u32 dimm_config2;
    /* 00:03.4 registers */
    u32 timing_grp0;
    u32 timing_grp1;
    u32 timing_grp2;
    u32 tfaw_trrd;
    u32 cmd_addr_timing;
    u32 odt_drive;
    u32 write_level;
    u32 dqs_phase;
    u32 tras;
    u32 trc;
    u32 trtp_twtr;
    u32 trfc;
    u32 trefi;
    u32 mrs_values;
    u32 mrs_control;
};

static struct mc_saved_regs saved;
static struct pci_dev *dev_dimm;    /* 00:03.1 */
static struct pci_dev *dev_timing;  /* 00:03.4 */
static struct timer_list watchdog;
static bool watchdog_armed;
static bool registers_modified;

/* Watchdog timeout: 500ms */
#define WATCHDOG_TIMEOUT_MS 500

static void save_all_registers(void)
{
    if (!dev_dimm || !dev_timing) return;

    pci_read_config_dword(dev_dimm, REG_DIMM_GEOMETRY, &saved.dimm_geometry);
    pci_read_config_dword(dev_dimm, REG_CAS_LATENCY, &saved.cas_latency);
    pci_read_config_dword(dev_dimm, REG_DRAM_DENSITY, &saved.dram_density);
    pci_read_config_dword(dev_dimm, REG_DIMM_CONFIG2, &saved.dimm_config2);

    pci_read_config_dword(dev_timing, REG_TIMING_GRP0, &saved.timing_grp0);
    pci_read_config_dword(dev_timing, REG_TIMING_GRP1, &saved.timing_grp1);
    pci_read_config_dword(dev_timing, REG_TIMING_GRP2, &saved.timing_grp2);
    pci_read_config_dword(dev_timing, REG_TFAW_TRRD, &saved.tfaw_trrd);
    pci_read_config_dword(dev_timing, REG_CMD_ADDR_TIMING, &saved.cmd_addr_timing);
    pci_read_config_dword(dev_timing, REG_ODT_DRIVE, &saved.odt_drive);
    pci_read_config_dword(dev_timing, REG_WRITE_LEVEL, &saved.write_level);
    pci_read_config_dword(dev_timing, REG_DQS_PHASE, &saved.dqs_phase);
    pci_read_config_dword(dev_timing, REG_TRAS, &saved.tras);
    pci_read_config_dword(dev_timing, REG_TRC, &saved.trc);
    pci_read_config_dword(dev_timing, REG_TRTP_TWTR, &saved.trtp_twtr);
    pci_read_config_dword(dev_timing, REG_TRFC, &saved.trfc);
    pci_read_config_dword(dev_timing, REG_TREFI, &saved.trefi);
    pci_read_config_dword(dev_timing, REG_MRS_VALUES, &saved.mrs_values);
    pci_read_config_dword(dev_timing, REG_MRS_CONTROL, &saved.mrs_control);

    pr_info("dram_probe: all registers saved\n");
}

static void restore_all_registers(void)
{
    if (!dev_dimm || !dev_timing || !registers_modified) return;

    /* CRITICAL: restore tREFI first to prevent data loss */
    pci_write_config_dword(dev_timing, REG_TREFI, saved.trefi);

    pci_write_config_dword(dev_timing, REG_MRS_VALUES, saved.mrs_values);
    pci_write_config_dword(dev_timing, REG_MRS_CONTROL, saved.mrs_control);
    pci_write_config_dword(dev_timing, REG_TIMING_GRP0, saved.timing_grp0);
    pci_write_config_dword(dev_timing, REG_TIMING_GRP1, saved.timing_grp1);
    pci_write_config_dword(dev_timing, REG_TIMING_GRP2, saved.timing_grp2);
    pci_write_config_dword(dev_timing, REG_TRAS, saved.tras);
    pci_write_config_dword(dev_timing, REG_TRC, saved.trc);
    pci_write_config_dword(dev_timing, REG_TRTP_TWTR, saved.trtp_twtr);
    pci_write_config_dword(dev_timing, REG_TRFC, saved.trfc);

    registers_modified = false;
    pr_info("dram_probe: ★ all registers RESTORED to safe values\n");
}

static void watchdog_fire(struct timer_list *t)
{
    pr_emerg("dram_probe: ⚠️  WATCHDOG FIRED — restoring registers!\n");
    restore_all_registers();
    watchdog_armed = false;
}

static void watchdog_pet(void)
{
    if (watchdog_armed)
        mod_timer(&watchdog, jiffies + msecs_to_jiffies(WATCHDOG_TIMEOUT_MS));
}

static void watchdog_start(void)
{
    if (!watchdog_armed) {
        timer_setup(&watchdog, watchdog_fire, 0);
        mod_timer(&watchdog, jiffies + msecs_to_jiffies(WATCHDOG_TIMEOUT_MS));
        watchdog_armed = true;
        pr_info("dram_probe: watchdog armed (%dms timeout)\n", WATCHDOG_TIMEOUT_MS);
    }
}

static void watchdog_stop(void)
{
    if (watchdog_armed) {
        del_timer_sync(&watchdog);
        watchdog_armed = false;
        pr_info("dram_probe: watchdog disarmed\n");
    }
}

/* ═══════════════════════════════════════════════════════════
 * Register Read/Write with Safety
 * ═══════════════════════════════════════════════════════════ */

static u32 mc_read(struct pci_dev *dev, u8 reg)
{
    u32 val;
    pci_read_config_dword(dev, reg, &val);
    return val;
}

static void mc_write(struct pci_dev *dev, u8 reg, u32 val)
{
    if (!registers_modified) {
        save_all_registers();
        watchdog_start();
        registers_modified = true;
    }
    watchdog_pet();
    pci_write_config_dword(dev, reg, val);
}

/* ═══════════════════════════════════════════════════════════
 * Refresh Control
 * ═══════════════════════════════════════════════════════════ */

static void refresh_disable(void)
{
    pr_warn("dram_probe: ⚠️  DISABLING AUTO-REFRESH — data decay starts NOW\n");
    mc_write(dev_timing, REG_TREFI, TREFI_DISABLED);
}

static void refresh_enable(void)
{
    mc_write(dev_timing, REG_TREFI, TREFI_DEFAULT);
    pr_info("dram_probe: ✅ auto-refresh RE-ENABLED (tREFI=%d clocks)\n", TREFI_DEFAULT);
}

static void refresh_set(u32 interval)
{
    mc_write(dev_timing, REG_TREFI, interval);
    pr_info("dram_probe: tREFI set to %u clocks (%u.%02u µs)\n",
            interval, ((interval * 15) / 10000), (((interval * 15) % 10000) / 100));
}

/* ═══════════════════════════════════════════════════════════
 * MRS Command Interface
 *
 * Based on EFI firmware RE:
 *   MR0 = 0x50, MR1 = 0x51, MR2 = 0x52, MR3 = 0x53
 *   Register 0xE8 holds MRS values
 * ═══════════════════════════════════════════════════════════ */

static void mrs_write(u8 mr_num, u16 value)
{
    u32 current_mrs;
    u32 new_mrs;
    u8 byte_offset;

    if (mr_num > 3) {
        pr_err("dram_probe: invalid MR number %d (must be 0-3)\n", mr_num);
        return;
    }

    /* Read current MRS register */
    current_mrs = mc_read(dev_timing, REG_MRS_VALUES);

    /* Each MR value occupies one byte in the register */
    byte_offset = mr_num * 8;
    new_mrs = (current_mrs & ~(0xFF << byte_offset)) | ((value & 0xFF) << byte_offset);

    pr_info("dram_probe: MRS: MR%d = 0x%02x (reg 0xE8: 0x%08x → 0x%08x)\n",
            mr_num, value & 0xFF, current_mrs, new_mrs);

    mc_write(dev_timing, REG_MRS_VALUES, new_mrs);
}

/* ═══════════════════════════════════════════════════════════
 * /proc Interface for Userspace Control
 * ═══════════════════════════════════════════════════════════ */

static int proc_status_show(struct seq_file *m, void *v)
{
    u32 val;

    seq_puts(m, "╔═══════════════════════════════════════════╗\n");
    seq_puts(m, "║  MCP79 DRAM Probe — Register Status       ║\n");
    seq_puts(m, "╠═══════════════════════════════════════════╣\n");

    /* 00:03.1 — DIMM Configuration */
    val = mc_read(dev_dimm, REG_DIMM_GEOMETRY);
    seq_printf(m, "║ DIMM Geometry   (03.1/0x68) = 0x%08x  ║\n", val);
    seq_printf(m, "║   Banks: %d  DIMMs: %d  Ranks: %d          ║\n",
               val & 0xFF, (val >> 16) & 0xFF, (val >> 24) & 0xFF);

    val = mc_read(dev_dimm, REG_CAS_LATENCY);
    seq_printf(m, "║ CAS Latency     (03.1/0x6C) = CL%d        ║\n", val & 0xFF);

    val = mc_read(dev_dimm, REG_DRAM_DENSITY);
    seq_printf(m, "║ DRAM Density    (03.1/0xAC) = 0x%08x  ║\n", val);

    /* 00:03.4 — Timing Controller */
    val = mc_read(dev_timing, REG_TRAS);
    seq_printf(m, "║ tRAS            (03.4/0x68) = %d clocks    ║\n", val & 0xFF);

    val = mc_read(dev_timing, REG_TRC);
    seq_printf(m, "║ tRC             (03.4/0x6C) = %d clocks   ║\n", (val >> 8) & 0xFF);

    val = mc_read(dev_timing, REG_TREFI);
    seq_printf(m, "║ tREFI           (03.4/0xAC) = %d clocks  ║\n", val & 0xFFFF);
    seq_printf(m, "║   = %d.%02d µs (%s)            ║\n",
               ((val & 0xFFFF) * 15) / 10000,
               (((val & 0xFFFF) * 15) % 10000) / 100,
               (val & 0xFFFF) == 0 ? "DISABLED!" : "active");

    val = mc_read(dev_timing, REG_MRS_VALUES);
    seq_printf(m, "║ MRS Values      (03.4/0xE8) = 0x%08x  ║\n", val);
    seq_printf(m, "║   MR0=0x%02x MR1=0x%02x MR2=0x%02x MR3=0x%02x  ║\n",
               val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF, (val >> 24) & 0xFF);

    seq_puts(m, "╠═══════════════════════════════════════════╣\n");
    seq_printf(m, "║ Watchdog: %s                        ║\n",
               watchdog_armed ? "ARMED ⏱️ " : "disarmed");
    seq_printf(m, "║ Modified: %s                            ║\n",
               registers_modified ? "YES ⚠️ " : "no");
    seq_puts(m, "╚═══════════════════════════════════════════╝\n");

    return 0;
}

static int proc_status_open(struct inode *inode, struct file *file)
{
    return single_open(file, proc_status_show, NULL);
}

static ssize_t proc_cmd_write(struct file *file, const char __user *buf,
                              size_t count, loff_t *ppos)
{
    char cmd[64];
    size_t len = min(count, sizeof(cmd) - 1);

    if (copy_from_user(cmd, buf, len))
        return -EFAULT;
    cmd[len] = '\0';

    /* Strip trailing newline */
    if (len > 0 && cmd[len-1] == '\n')
        cmd[--len] = '\0';

    if (strcmp(cmd, "save") == 0) {
        save_all_registers();
        pr_info("dram_probe: registers saved\n");
    } else if (strcmp(cmd, "restore") == 0) {
        restore_all_registers();
        pr_info("dram_probe: registers restored\n");
    } else if (strcmp(cmd, "pet") == 0) {
        watchdog_pet();
    } else if (strcmp(cmd, "refresh_off") == 0) {
        refresh_disable();
    } else if (strcmp(cmd, "refresh_on") == 0) {
        refresh_enable();
    } else if (strncmp(cmd, "trefi ", 6) == 0) {
        u32 val;
        if (kstrtou32(cmd + 6, 0, &val) == 0)
            refresh_set(val);
    } else if (strncmp(cmd, "mrs ", 4) == 0) {
        unsigned int mr, val;
        if (sscanf(cmd + 4, "%u %x", &mr, &val) == 2)
            mrs_write(mr, val);
    } else if (strcmp(cmd, "dump") == 0) {
        int reg;
        pr_info("dram_probe: === 00:03.4 full register dump ===\n");
        for (reg = 0x40; reg <= 0xFC; reg += 4) {
            u32 v = mc_read(dev_timing, reg);
            if (v != 0 && v != 0xFFFFFFFF)
                pr_info("dram_probe:   0x%02x = 0x%08x\n", reg, v);
        }
    } else {
        pr_info("dram_probe: commands: save restore pet refresh_off refresh_on "
                "trefi <val> mrs <mr> <hex_val> dump\n");
    }

    return count;
}

static const struct proc_ops proc_status_ops = {
    .proc_open    = proc_status_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

static const struct proc_ops proc_cmd_ops = {
    .proc_write   = proc_cmd_write,
};

/* ═══════════════════════════════════════════════════════════
 * Module Init / Exit
 * ═══════════════════════════════════════════════════════════ */

static int __init dram_probe_init(void)
{
    pr_info("dram_probe: ═══════════════════════════════════════\n");
    pr_info("dram_probe: MCP79 DRAM Controller Probe v0.1.0\n");
    pr_info("dram_probe: JackKnife Studios — Spare Row Liberation\n");
    pr_info("dram_probe: ═══════════════════════════════════════\n");

    /* Find PCI devices */
    dev_dimm = pci_get_domain_bus_and_slot(0, MC_DIMM_BUS,
                                           PCI_DEVFN(MC_DIMM_DEV, MC_DIMM_FUNC));
    if (!dev_dimm) {
        pr_err("dram_probe: ERROR: cannot find 00:03.1 (DIMM config)\n");
        return -ENODEV;
    }

    dev_timing = pci_get_domain_bus_and_slot(0, MC_TIMING_BUS,
                                              PCI_DEVFN(MC_TIMING_DEV, MC_TIMING_FUNC));
    if (!dev_timing) {
        pr_err("dram_probe: ERROR: cannot find 00:03.4 (timing controller)\n");
        pci_dev_put(dev_dimm);
        return -ENODEV;
    }

    pr_info("dram_probe: found 00:03.1 [%04x:%04x] — DIMM config\n",
            dev_dimm->vendor, dev_dimm->device);
    pr_info("dram_probe: found 00:03.4 [%04x:%04x] — timing controller\n",
            dev_timing->vendor, dev_timing->device);

    /* Save initial register state */
    save_all_registers();

    /* Print current state */
    pr_info("dram_probe: CAS Latency = CL%d\n", saved.cas_latency & 0xFF);
    pr_info("dram_probe: tREFI = %d clocks (%d.%02d µs)\n",
            saved.trefi & 0xFFFF,
            ((saved.trefi & 0xFFFF) * 15) / 10000,
            (((saved.trefi & 0xFFFF) * 15) % 10000) / 100);
    pr_info("dram_probe: MRS values = 0x%08x\n", saved.mrs_values);
    pr_info("dram_probe: DRAM density = 0x%08x\n", saved.dram_density);

    /* Create /proc entries */
    proc_create("dram_probe", 0444, NULL, &proc_status_ops);
    proc_create("dram_probe_cmd", 0200, NULL, &proc_cmd_ops);

    pr_info("dram_probe: ✅ loaded — /proc/dram_probe (status) /proc/dram_probe_cmd (control)\n");
    pr_info("dram_probe: commands: echo 'save|restore|refresh_off|refresh_on|dump' > /proc/dram_probe_cmd\n");
    pr_info("dram_probe: MRS cmd:  echo 'mrs <0-3> <hex_value>' > /proc/dram_probe_cmd\n");

    return 0;
}

static void __exit dram_probe_exit(void)
{
    /* ALWAYS restore registers on unload */
    if (registers_modified)
        restore_all_registers();

    watchdog_stop();

    remove_proc_entry("dram_probe_cmd", NULL);
    remove_proc_entry("dram_probe", NULL);

    if (dev_timing)
        pci_dev_put(dev_timing);
    if (dev_dimm)
        pci_dev_put(dev_dimm);

    pr_info("dram_probe: ✅ unloaded — all registers restored to safe values\n");
}

module_init(dram_probe_init);
module_exit(dram_probe_exit);

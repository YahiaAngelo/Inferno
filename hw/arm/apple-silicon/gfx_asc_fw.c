/*
 * Apple GPU firmware coprocessor (gfx-asc), firmware-running variant.
 *
 * Lives here rather than next to the logging shell in hw/misc because it owns
 * an ARM CPU and needs the target-specific CPU definitions.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/a13.h"
#include "hw/misc/apple-silicon/a7iop/core.h"
#include "hw/misc/apple-silicon/gfx_asc.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "target/arm/cpu.h"

/*
 * Firmware-running variant.
 *
 * The guest writes (firmware region base | 1) to the core register and then
 * starts the coprocessor through the A7IOP CPU control register. At that
 * point the firmware's segments are copied to the region, as iBoot would have
 * done, and the coprocessor's CPU is released from reset at the region base.
 * The CPU sees the SoC's physical memory, like every A13-class CPU here.
 */

#define TYPE_APPLE_GFX_ASC_FW "apple-gfx-asc-fw"
OBJECT_DECLARE_TYPE(AppleGFXASCFWState, AppleGFXASCFWClass, APPLE_GFX_ASC_FW)

#define GFX_ASC_CORE_REG_START BIT_ULL(0)

#define MACHO_MAGIC_64 (0xFEEDFACF)
#define MACHO_LC_SEGMENT_64 (0x19)
#define MACHO_HEADER_64_SIZE (32)

struct AppleGFXASCFWClass {
    /*< private >*/
    SysBusDeviceClass parent_class;

    /*< public >*/
    DeviceRealize parent_realize;
};

struct AppleGFXASCFWState {
    /*< private >*/
    AppleA7IOP parent_obj;

    /*< public >*/
    MemoryRegion ascv2_iomem;
    ARMCPU *cpu;
    uint64_t core_reg;
    uint8_t *fw;
    gsize fw_size;
    hwaddr load_addr;
    hwaddr wrapper_base;
    hwaddr gptbat_base;
    hwaddr ppl_shared_base;
};

/*
 * The firmware carries a table of values the bootloader fills in before
 * starting it (the __rtk_patch_* symbols). Each entry is a four-character
 * tag, a 32-bit length and the value, unaligned.
 *
 * The start code reads PTPB/PTPS (memory for its page tables: it points
 * TTBR at PTPB and builds tables there on a cold boot) and GptS. The rest
 * are read later.
 *
 * FIXME: which entries iBoot really patches, and with what, is inferred from
 * the names and from how the start code uses them. Entries not listed here
 * keep the values in the file.
 */
#define GFX_FW_PAGE_TABLE_OFFSET (2 * MiB)
#define GFX_FW_PAGE_TABLE_SIZE (1 * MiB)
#define GFX_FW_TIMER_FREQUENCY (24000000)

static bool apple_gfx_asc_fw_patch(uint8_t *image, size_t size,
                                   const char *tag, uint32_t len,
                                   uint64_t value)
{
    uint8_t key[8];
    uint8_t *found = NULL;
    size_t i;

    memcpy(key, tag, 4);
    stl_le_p(key + 4, len);
    for (i = 0; i + sizeof(key) + len <= size; i++) {
        if (memcmp(image + i, key, sizeof(key)) == 0) {
            if (found != NULL) {
                return false; // ambiguous
            }
            found = image + i;
        }
    }
    if (found == NULL) {
        return false;
    }
    if (len == 8) {
        stq_le_p(found + sizeof(key), value);
    } else if (len == 4) {
        stl_le_p(found + sizeof(key), value);
    } else {
        return false;
    }
    return true;
}

static void apple_gfx_asc_fw_patch_all(AppleGFXASCFWState *s, uint8_t *image,
                                       hwaddr base, uint64_t first_vm)
{
    hwaddr pt_base = base + GFX_FW_PAGE_TABLE_OFFSET;
    const struct {
        const char *tag;
        uint32_t len;
        uint64_t value;
    } patches[] = {
        { "dApC", 8, base }, // CPU physical address
        { "dArW", 8, s->wrapper_base }, // CPU wrapper physical address
        { "BPTP", 8, pt_base }, // page table base
        { "SPTP", 8, GFX_FW_PAGE_TABLE_SIZE }, // page table size
        { "BVTP", 8, first_vm + GFX_FW_PAGE_TABLE_OFFSET }, // ...virtual base
        { "BtpG", 8, s->gptbat_base }, // UAT GPTBAT base
        { "BlpP", 8, s->ppl_shared_base }, // UAT PPL shared base
        { "qF8v", 4, GFX_FW_TIMER_FREQUENCY }, // ARMv8 timer frequency
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(patches); i++) {
        if (!apple_gfx_asc_fw_patch(image, s->fw_size, patches[i].tag,
                                    patches[i].len, patches[i].value)) {
            warn_report("gfx-asc: could not patch firmware entry '%s'",
                        patches[i].tag);
        }
    }
}

// Copies every LC_SEGMENT_64 of a patched copy of the image to `base`,
// relative to the lowest segment address.
static bool apple_gfx_asc_fw_load(AppleGFXASCFWState *s, hwaddr base)
{
    g_autofree uint8_t *m = g_memdup2(s->fw, s->fw_size);
    uint64_t first_vm = UINT64_MAX;
    uint32_t ncmds;
    size_t off;
    uint32_t i;
    int pass;

    if (s->fw_size < MACHO_HEADER_64_SIZE || ldl_le_p(m) != MACHO_MAGIC_64) {
        return false;
    }
    ncmds = ldl_le_p(m + 16);

    for (pass = 0; pass < 2; pass++) {
        off = MACHO_HEADER_64_SIZE;
        for (i = 0; i < ncmds; i++) {
            uint32_t cmd, cmdsize;
            uint64_t vmaddr, fileoff, filesize;

            if (off + 8 > s->fw_size) {
                return false;
            }
            cmd = ldl_le_p(m + off);
            cmdsize = ldl_le_p(m + off + 4);
            if (cmdsize < 8 || off + cmdsize > s->fw_size) {
                return false;
            }
            if (cmd == MACHO_LC_SEGMENT_64 && cmdsize >= 72) {
                vmaddr = ldq_le_p(m + off + 24);
                fileoff = ldq_le_p(m + off + 40);
                filesize = ldq_le_p(m + off + 48);
                if (pass == 0) {
                    first_vm = MIN(first_vm, vmaddr);
                } else if (filesize != 0) {
                    if (fileoff > s->fw_size ||
                        filesize > s->fw_size - fileoff) {
                        return false;
                    }
                    if (address_space_write(&address_space_memory,
                                            base + (vmaddr - first_vm),
                                            MEMTXATTRS_UNSPECIFIED,
                                            m + fileoff,
                                            filesize) != MEMTX_OK) {
                        return false;
                    }
                }
            }
            off += cmdsize;
        }
        if (first_vm == UINT64_MAX) {
            return false;
        }
        if (pass == 0) {
            apple_gfx_asc_fw_patch_all(s, m, base, first_vm);
        }
    }
    return true;
}

// Runs on the coprocessor's CPU after it has been reset and powered on, before
// it executes anything.
static void apple_gfx_asc_fw_cpu_init_work(CPUState *cpu, run_on_cpu_data data)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;

    // rtk_start_anywhere_uat uses an FP register (fmov d17, x0) one
    // instruction before it writes CPACR_EL1 itself, so FP access must
    // already be enabled out of reset or the core takes a trap into its own
    // vector table and spins there.
    env->cp15.cpacr_el1 = deposit64(env->cp15.cpacr_el1, 20, 2, 3);
    arm_rebuild_hflags(env);
}

static void apple_gfx_asc_fw_start(AppleA7IOP *a7iop)
{
    AppleGFXASCFWState *s = container_of(a7iop, AppleGFXASCFWState, parent_obj);

    if ((s->core_reg & GFX_ASC_CORE_REG_START) == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gfx-asc: start requested before the core register was "
                      "written\n");
        return;
    }

    s->load_addr = s->core_reg & ~0xFFFULL;
    if (!apple_gfx_asc_fw_load(s, s->load_addr)) {
        error_report("gfx-asc: failed to load the firmware at 0x%" HWADDR_PRIx,
                     s->load_addr);
        return;
    }

    object_property_set_uint(OBJECT(s->cpu), "rvbar", s->load_addr,
                             &error_abort);
    apple_a13_set_on(APPLE_A13(s->cpu));
    async_run_on_cpu(CPU(s->cpu), apple_gfx_asc_fw_cpu_init_work,
                     RUN_ON_CPU_NULL);
    info_report("gfx-asc: firmware loaded and started at 0x%" HWADDR_PRIx,
                s->load_addr);
}

static void apple_gfx_asc_fw_wakeup(AppleA7IOP *a7iop)
{
    qemu_log_mask(LOG_UNIMP, "gfx-asc: wakeup\n");
}

static const AppleA7IOPOps apple_gfx_asc_fw_iop_ops = {
    .start = apple_gfx_asc_fw_start,
    .wakeup = apple_gfx_asc_fw_wakeup,
};

static void apple_gfx_asc_fw_core_reg_write(void *opaque, hwaddr addr,
                                            uint64_t data, unsigned size)
{
    AppleGFXASCFWState *s = opaque;

    qemu_log_mask(LOG_UNIMP,
                  "gfx-asc: core reg write (size %u, offset 0x%" HWADDR_PRIx
                  ", value 0x%" PRIx64 ")\n",
                  size, addr, data);
    if (addr == 0) {
        s->core_reg = data;
    }
}

static uint64_t apple_gfx_asc_fw_core_reg_read(void *opaque, hwaddr addr,
                                               unsigned size)
{
    AppleGFXASCFWState *s = opaque;

    return addr == 0 ? s->core_reg : 0;
}

static const MemoryRegionOps apple_gfx_asc_fw_core_reg_ops = {
    .write = apple_gfx_asc_fw_core_reg_write,
    .read = apple_gfx_asc_fw_core_reg_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .valid.unaligned = false,
};

static void apple_gfx_asc_fw_realize(DeviceState *dev, Error **errp)
{
    AppleGFXASCFWState *s = APPLE_GFX_ASC_FW(dev);
    AppleGFXASCFWClass *c = APPLE_GFX_ASC_FW_GET_CLASS(dev);
    DeviceState *mailbox;

    if (c->parent_realize != NULL) {
        c->parent_realize(dev, errp);
    }

    qdev_realize(DEVICE(s->cpu), NULL, errp);

    // Same wiring the SEP uses: the coprocessor-side mailbox interrupt goes to
    // the CPU, and the CPU's timers go back into the mailbox's controller.
    mailbox = DEVICE(s->parent_obj.iop_mailbox);
    qdev_connect_gpio_out_named(mailbox, APPLE_A7IOP_SEP_CPU_IRQ, 0,
                                qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    qdev_connect_gpio_out(DEVICE(s->cpu), GTIMER_PHYS,
                          qdev_get_gpio_in_named(mailbox,
                                                 APPLE_A7IOP_SEP_GPIO_TIMER0,
                                                 0));
    qdev_connect_gpio_out(DEVICE(s->cpu), GTIMER_VIRT,
                          qdev_get_gpio_in_named(mailbox,
                                                 APPLE_A7IOP_SEP_GPIO_TIMER1,
                                                 0));
}

static void apple_gfx_asc_fw_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AppleGFXASCFWClass *c = APPLE_GFX_ASC_FW_CLASS(klass);

    device_class_set_parent_realize(dc, apple_gfx_asc_fw_realize,
                                    &c->parent_realize);
    dc->desc = "Apple GPU firmware coprocessor";
}

static const TypeInfo apple_gfx_asc_fw_info = {
    .name = TYPE_APPLE_GFX_ASC_FW,
    .parent = TYPE_APPLE_A7IOP,
    .instance_size = sizeof(AppleGFXASCFWState),
    .class_size = sizeof(AppleGFXASCFWClass),
    .class_init = apple_gfx_asc_fw_class_init,
};

static void apple_gfx_asc_fw_register_types(void)
{
    type_register_static(&apple_gfx_asc_fw_info);
}

type_init(apple_gfx_asc_fw_register_types);

void apple_gfx_asc_fw_set_platform(SysBusDevice *sbd, hwaddr wrapper_base,
                                   hwaddr gptbat_base, hwaddr ppl_shared_base)
{
    AppleGFXASCFWState *s = APPLE_GFX_ASC_FW(sbd);

    s->wrapper_base = wrapper_base;
    s->gptbat_base = gptbat_base;
    s->ppl_shared_base = ppl_shared_base;
}

SysBusDevice *apple_gfx_asc_fw_create(AppleDTNode *node,
                                      AppleA7IOPVersion version,
                                      uint32_t cpu_id, const char *fw_filename)
{
    DeviceState *dev;
    AppleGFXASCFWState *s;
    SysBusDevice *sbd;
    AppleDTNode *child;
    AppleDTProp *prop;
    uint64_t *reg;
    gchar *fw = NULL;

    dev = qdev_new(TYPE_APPLE_GFX_ASC_FW);
    s = APPLE_GFX_ASC_FW(dev);
    sbd = SYS_BUS_DEVICE(dev);
    dev->id = g_strdup("gfx-asc");

    child = apple_dt_get_node(node, "iop-gfx-nub");
    assert_nonnull(child);

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    apple_a7iop_init(&s->parent_obj, "GFX", reg[1], version,
                     &apple_gfx_asc_fw_iop_ops, NULL);

    memory_region_init_io(&s->ascv2_iomem, OBJECT(dev),
                          &apple_gfx_asc_fw_core_reg_ops, s,
                          TYPE_APPLE_GFX_ASC_FW ".ascv2-core-reg", reg[3]);
    sysbus_init_mmio(sbd, &s->ascv2_iomem);

    if (!g_file_get_contents(fw_filename, &fw, &s->fw_size, NULL)) {
        error_setg(&error_fatal, "Could not load GPU firmware from `%s`",
                   fw_filename);
        return NULL;
    }
    s->fw = (uint8_t *)fw;
    if (s->fw_size < MACHO_HEADER_64_SIZE ||
        ldl_le_p(s->fw) != MACHO_MAGIC_64) {
        error_setg(&error_fatal,
                   "`%s` is not a 64-bit Mach-O (unwrap the im4p first)",
                   fw_filename);
        return NULL;
    }

    // The physical ID only has to be unique; the SEP's CPU uses bit 30.
    s->cpu = &apple_a13_create("gfx-cpu", cpu_id, BIT(29), -1, 'G')->parent_obj;
    // The firmware's bootstrap aborts with "!MIDR" unless the part number is
    // one it knows: 0x00d, 0x00e, 0x00f, 0x014, 0x015, 0x026, 0x240..0x245 or
    // 0x250..0x252. FIXME: the real GPU coprocessor's part number is unknown;
    // 0x15 is on that list and is what this SoC's SEP core model uses.
    s->cpu->midr = deposit64(s->cpu->midr, 4, 12, 0x15); // MIDR_EL1.PartNum
    object_property_add_child(OBJECT(dev), DEVICE(s->cpu)->id, OBJECT(s->cpu));

    apple_dt_set_prop_u32(child, "pre-loaded", 1);
    apple_dt_set_prop_u32(child, "running", 1);

    return sbd;
}

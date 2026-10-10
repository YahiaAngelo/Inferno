/*
 * Apple GPU firmware coprocessor (gfx-asc), logging shell.
 *
 * On hardware iBoot loads the GPU firmware (Firmware/agx/armfw_*.im4p, an
 * RTKit image) onto this coprocessor, and the guest's AGX driver expects it to
 * be running already: AGXFirmwareKextRTBuddy::bootFirmware() returns early
 * only when the RTBuddy service reports the firmware as started.
 *
 * This is not that firmware. It answers the RTKit management protocol, marks
 * the nub as pre-loaded and running, advertises the two user endpoints and
 * logs what the guest sends them (`-d unimp`), so the driver's side of the
 * protocol can be observed. Nothing is answered on the user endpoints.
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
#include "hw/misc/apple-silicon/a7iop/rtkit.h"
#include "hw/misc/apple-silicon/gfx_asc.h"
#include "qapi/error.h"
#include "qemu/log.h"

// User endpoints are numbered from 0 here; RTKit adds its user base (0x20).
// The Asahi Linux driver uses 0x20 (firmware control) and 0x21 (doorbell) on
// M1. FIXME: not confirmed for G12.
#define GFX_ASC_EP_FIRMWARE (0)
#define GFX_ASC_EP_DOORBELL (1)

struct AppleGFXASCState {
    /*< private >*/
    AppleRTKit parent_obj;

    /*< public >*/
    MemoryRegion ascv2_iomem;
};

static void apple_gfx_asc_ep_handler(void *opaque, uint8_t ep, uint64_t msg)
{
    qemu_log_mask(LOG_UNIMP,
                  "gfx-asc: endpoint %u received 0x%016" PRIx64 "\n", ep, msg);
}

static void apple_gfx_asc_start(void *opaque)
{
    qemu_log_mask(LOG_UNIMP, "gfx-asc: start\n");
}

static void apple_gfx_asc_wakeup(void *opaque)
{
    qemu_log_mask(LOG_UNIMP, "gfx-asc: wakeup\n");
}

static void apple_gfx_asc_boot_done(void *opaque)
{
    qemu_log_mask(LOG_UNIMP, "gfx-asc: boot done\n");
}

static const AppleRTKitOps apple_gfx_asc_rtkit_ops = {
    .start = apple_gfx_asc_start,
    .wakeup = apple_gfx_asc_wakeup,
    .boot_done = apple_gfx_asc_boot_done,
};

static void ascv2_core_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                 unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "gfx-asc: core reg write (size %u, offset 0x%" HWADDR_PRIx
                  ", value 0x%" PRIx64 ")\n",
                  size, addr, data);
}

static uint64_t ascv2_core_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "gfx-asc: core reg read  (size %u, offset 0x%" HWADDR_PRIx
                  ")\n",
                  size, addr);
    return 0;
}

static const MemoryRegionOps ascv2_core_reg_ops = {
    .write = ascv2_core_reg_write,
    .read = ascv2_core_reg_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .valid.unaligned = false,
};

static const TypeInfo apple_gfx_asc_info = {
    .name = TYPE_APPLE_GFX_ASC,
    .parent = TYPE_APPLE_RTKIT,
    .instance_size = sizeof(AppleGFXASCState),
};

static void apple_gfx_asc_register_types(void)
{
    type_register_static(&apple_gfx_asc_info);
}

type_init(apple_gfx_asc_register_types);

SysBusDevice *apple_gfx_asc_create(AppleDTNode *node,
                                   AppleA7IOPVersion version)
{
    DeviceState *dev;
    AppleGFXASCState *s;
    SysBusDevice *sbd;
    AppleRTKit *rtk;
    AppleDTNode *child;
    AppleDTProp *prop;
    uint64_t *reg;

    dev = qdev_new(TYPE_APPLE_GFX_ASC);
    s = APPLE_GFX_ASC(dev);
    sbd = SYS_BUS_DEVICE(dev);
    rtk = APPLE_RTKIT(dev);
    dev->id = g_strdup("gfx-asc");

    child = apple_dt_get_node(node, "iop-gfx-nub");
    assert_nonnull(child);

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);

    reg = (uint64_t *)prop->data;

    apple_rtkit_init(rtk, s, "GFX", reg[1], version, &apple_gfx_asc_rtkit_ops);

    memory_region_init_io(&s->ascv2_iomem, OBJECT(dev), &ascv2_core_reg_ops, s,
                          TYPE_APPLE_GFX_ASC ".ascv2-core-reg", reg[3]);
    sysbus_init_mmio(sbd, &s->ascv2_iomem);

    apple_rtkit_register_user_ep(rtk, GFX_ASC_EP_FIRMWARE, s,
                                 apple_gfx_asc_ep_handler);
    apple_rtkit_register_user_ep(rtk, GFX_ASC_EP_DOORBELL, s,
                                 apple_gfx_asc_ep_handler);

    apple_dt_set_prop_u32(child, "pre-loaded", 1);
    apple_dt_set_prop_u32(child, "running", 1);

    return sbd;
}

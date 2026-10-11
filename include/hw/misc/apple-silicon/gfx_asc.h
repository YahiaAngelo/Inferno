/*
 * Apple GPU firmware coprocessor (gfx-asc), logging shell.
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

#ifndef HW_MISC_APPLE_SILICON_GFX_ASC_H
#define HW_MISC_APPLE_SILICON_GFX_ASC_H

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dt.h"
#include "hw/misc/apple-silicon/a7iop/base.h"
#include "hw/sysbus.h"

#define TYPE_APPLE_GFX_ASC "apple-gfx-asc"
OBJECT_DECLARE_SIMPLE_TYPE(AppleGFXASCState, APPLE_GFX_ASC)

SysBusDevice *apple_gfx_asc_create(AppleDTNode *node,
                                   AppleA7IOPVersion version);

// Variant that runs the real GPU firmware (an RTKit Mach-O unwrapped from
// Firmware/agx/armfw_*.im4p) on its own CPU instead of logging.
SysBusDevice *apple_gfx_asc_fw_create(AppleDTNode *node,
                                      AppleA7IOPVersion version,
                                      uint32_t cpu_id, const char *fw_filename);
// Addresses the bootloader patches into the firmware before starting it.
/*
 * Raise external interrupt @irq on the GPU coprocessor's interrupt
 * controller. The firmware unmasks 0 and 4 while the GPU is powered
 * (101..104 are the mailbox overflow/underflow interrupts).
 */
void apple_gfx_asc_fw_raise_irq(SysBusDevice *sbd, uint32_t irq);
void apple_gfx_asc_fw_set_platform(SysBusDevice *sbd, hwaddr wrapper_base,
                                   hwaddr page_table_base,
                                   hwaddr page_table_size, hwaddr gptbat_base,
                                   hwaddr ppl_shared_base);

#endif /* HW_MISC_APPLE_SILICON_GFX_ASC_H */

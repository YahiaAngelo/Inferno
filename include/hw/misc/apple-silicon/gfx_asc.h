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

#endif /* HW_MISC_APPLE_SILICON_GFX_ASC_H */

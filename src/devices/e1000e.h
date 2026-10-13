/* Intel 82574L compatible Ethernet controller. SPDX-License-Identifier: MPL-2.0 */
#ifndef RVVM_E1000E_H
#define RVVM_E1000E_H

#include "tap_api.h"
#include <rvvm/rvvm_pci.h>

/* Takes ownership of tap, including on failure. */
RVVM_PUBLIC rvvm_pci_func_t* rvvm_e1000e_init(rvvm_machine_t* machine, tap_dev_t* tap, rvvm_pci_addr_t addr);

#endif

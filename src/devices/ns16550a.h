/*
ns16550a.h - NS16550A UART
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_NS16550A_H
#define RVVM_NS16550A_H

#include <rvvm/rvvm_char.h>

#include "chardev.h"

RVVM_PUBLIC rvvm_reg_dev_t* rvvm_ns16550a_init(rvvm_machine_t* machine, /**/
                                               chardev_t*      chardev, /**/
                                               rvvm_addr_t     addr,    /**/
                                               uint32_t        attr,    /**/
                                               rvvm_irq_dev_t* irq_dev, /**/
                                               rvvm_irq_t      irq);

rvvm_reg_dev_t* ns16550a_init_k1(rvvm_machine_t* machine, chardev_t* chardev);
rvvm_reg_dev_t* ns16550a_init_jh7110(rvvm_machine_t* machine, chardev_t* chardev);

static inline rvvm_reg_dev_t* ns16550a_init_auto(rvvm_machine_t* machine, chardev_t* chardev)
{
    return rvvm_ns16550a_init(machine, chardev, 0x10000000UL, 0, NULL, 0);
}

static inline rvvm_reg_dev_t* ns16550a_init_term_auto(rvvm_machine_t* machine)
{
    return ns16550a_init_auto(machine, chardev_term_create());
}

#endif

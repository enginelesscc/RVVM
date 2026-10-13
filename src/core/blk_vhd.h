/*
blk_vhd.h - VHD block image support
SPDX-License-Identifier: MPL-2.0
*/

#ifndef RVVM_BLK_VHD_H
#define RVVM_BLK_VHD_H

#include <rvvm/rvvm_blk.h>

bool                       rvvm_blk_vhd_detect(const char* path);
extern const rvvm_blk_cb_t rvvm_blk_vhd_cb;

#endif

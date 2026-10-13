/*
<rvvm/rvvm_blk.h> - RVVM Block device IO
Copyright (C) 2020-2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_BLKDEV_API_H
#define _RVVM_BLKDEV_API_H

#include <rvvm/rvvm_base.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_blk_api Block device API
 * @addtogroup rvvm_blk_api
 * @{
 */

/*
 * Options for rvvm_blk_open()
 */

#define RVVM_BLK_READ     0x01 /**< Permit reading the block device image      */
#define RVVM_BLK_WRITE    0x02 /**< Permit writing the block device image      */
#define RVVM_BLK_RW       0x03 /**< Open block device image in read/write mode */
#define RVVM_BLK_CREAT    0x04 /**< Create image if it doesn't exist           */
#define RVVM_BLK_EXCL     0x08 /**< Prevent other processes from opening       */
#define RVVM_BLK_TRUNC    0x10 /**< Truncate conents upon opening              */
#define RVVM_BLK_DIRECT   0x20 /**< Direct DMA IO with the underlying storage  */
#define RVVM_BLK_SYNC     0x40 /**< Disable writeback buffering                */
#define RVVM_BLK_GROW     0x80 /**< Allow writes beyond end of image           */

/*
 * Seek whence for rvvm_blk_seek_head()
 */
#define RVVM_BLK_SEEK_SET 0x00 /** Set drive head position relative to image beginning  */
#define RVVM_BLK_SEEK_CUR 0x01 /** Set drive head position relative to current position */
#define RVVM_BLK_SEEK_END 0x02 /** Set drive head position relative to image end        */

/**
 * Open a block device image
 *
 * \param name Image name (File path, etc)
 * \param fmt  "raw", "vhd" (fixed/dynamic), or NULL to auto-detect
 * \param opts Open options (Read/Write/Create), zero implies RVVM_BLK_RW | RVVM_BLK_DIRECT
 * \return     Block device image handle, or NULL on failure
 */
RVVM_PUBLIC rvvm_blk_dev_t* rvvm_blk_open(const char* name, const char* fmt, uint32_t opts);

/**
 * Close a block device image handle
 */
RVVM_PUBLIC void rvvm_blk_close(rvvm_blk_dev_t* blk);

/**
 * Get block device format identifier string
 */
RVVM_PUBLIC const char* rvvm_blk_get_format(rvvm_blk_dev_t* blk);

/**
 * Get block device size in bytes
 */
RVVM_PUBLIC uint64_t rvvm_blk_get_size(rvvm_blk_dev_t* blk);

/**
 * Resize block device (If supported)
 */
RVVM_PUBLIC bool rvvm_blk_set_size(rvvm_blk_dev_t* blk, uint64_t size);

/**
 * Read data at specified offset, returns how much was actually read
 */
RVVM_PUBLIC size_t rvvm_blk_read(rvvm_blk_dev_t* blk, void* dst, size_t size, uint64_t off);

/**
 * Write data at specified offset, returns how much was actually written
 */
RVVM_PUBLIC size_t rvvm_blk_write(rvvm_blk_dev_t* blk, const void* src, size_t size, uint64_t off);

/**
 * Synchronize / Barrier buffered writes to stable storage
 */
RVVM_PUBLIC bool rvvm_blk_sync(rvvm_blk_dev_t* blk);

/**
 * Trim (Deallocate) unused data (If supported)
 */
RVVM_PUBLIC bool rvvm_blk_trim(rvvm_blk_dev_t* blk, uint64_t off, uint64_t size);

/**
 * Seek drive head
 */
RVVM_PUBLIC bool rvvm_blk_seek_head(rvvm_blk_dev_t* blk, int64_t off, uint32_t whence);

/**
 * Tell drive head position
 */
RVVM_PUBLIC uint64_t rvvm_blk_tell_head(rvvm_blk_dev_t* blk);

/**
 * Read data at drive head, returns how much was actually read
 */
RVVM_PUBLIC size_t rvvm_blk_read_head(rvvm_blk_dev_t* blk, void* dst, size_t size);

/**
 * Write data at drive head, returns how much was actually written
 */
RVVM_PUBLIC size_t rvvm_blk_write_head(rvvm_blk_dev_t* blk, const void* src, size_t size);

/** @}*/

/**
 * @defgroup rvvm_blk_format_api Block device image format API
 * @addtogroup rvvm_blk_format_api
 * @{
 */

/**
 * Block device format handlers
 */
typedef struct {
    /**
     * Block device format identifier
     */
    const char* name;

    /**
     * Probe block device format
     */
    bool (*probe)(rvvm_blk_dev_t* blk, const char* name, uint32_t opts);

    /**
     * Free private block device data
     */
    void (*close)(rvvm_blk_dev_t* blk);

    /**
     * Get block device size in bytes
     */
    uint64_t (*get_size)(rvvm_blk_dev_t* blk);

    /**
     * Resize block device (If supported)
     */
    bool (*set_size)(rvvm_blk_dev_t* blk, uint64_t size);

    /**
     * Read data at specified offset, returns how much was actually read
     */
    size_t (*read)(rvvm_blk_dev_t* blk, void* dst, size_t size, uint64_t off);

    /**
     * Write data at specified offset, returns how much was actually written
     */
    size_t (*write)(rvvm_blk_dev_t* blk, const void* src, size_t size, uint64_t off);

    /**
     * Synchronize / Barrier buffered writes to stable storage
     */
    bool (*sync)(rvvm_blk_dev_t* blk);

    /**
     * Trim (Deallocate) unused data (If supported)
     */
    bool (*trim)(rvvm_blk_dev_t* blk, uint64_t off, uint64_t size);
} rvvm_blk_cb_t;

/**
 * Create a dummy block device
 */
RVVM_PUBLIC rvvm_blk_dev_t* rvvm_blk_init(void);

/**
 * Register block device image callbacks
 */
RVVM_PUBLIC void rvvm_blk_register_cb(rvvm_blk_dev_t* blk, const rvvm_blk_cb_t* cb, uint32_t opts);

/**
 * Set private block device image data
 */
RVVM_PUBLIC void rvvm_blk_set_data(rvvm_blk_dev_t* blk, void* data);

/**
 * Get private block device image data
 */
RVVM_PUBLIC void* rvvm_blk_get_data(rvvm_blk_dev_t* blk);

/** @}*/

RVVM_EXTERN_C_END

#endif

/*
rvvm_blk.c - RVVM Block device IO
Copyright (C) 2020-2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <rvvm/rvvm_blk.h>

#include "blk_io.h"
#include "blk_vhd.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

/*
 * Raw block device images
 */

static bool blk_raw_probe(rvvm_blk_dev_t* blk, const char* name, uint32_t opts)
{
    rvfile_t* file = rvopen(name, opts);
    if (file) {
        rvvm_blk_set_data(blk, file);
        return true;
    }
    return false;
}

static void blk_raw_close(rvvm_blk_dev_t* blk)
{
    rvfile_t* file = rvvm_blk_get_data(blk);
    rvclose(file);
}

static uint64_t blk_raw_get_size(rvvm_blk_dev_t* blk)
{
    rvfile_t* file = rvvm_blk_get_data(blk);
    return rvfilesize(file);
}

static bool blk_raw_set_size(rvvm_blk_dev_t* blk, uint64_t size)
{
    rvfile_t* file = rvvm_blk_get_data(blk);
    return rvtruncate(file, size);
}

static size_t blk_raw_read(rvvm_blk_dev_t* blk, void* dst, size_t size, uint64_t off)
{
    rvfile_t* file = rvvm_blk_get_data(blk);
    if (off != RVFILE_POSITION) {
        return rvread(file, dst, size, off);
    }
    return 0;
}

static size_t blk_raw_write(rvvm_blk_dev_t* blk, const void* src, size_t size, uint64_t off)
{
    rvfile_t* file = rvvm_blk_get_data(blk);
    if (off != RVFILE_POSITION) {
        return rvwrite(file, src, size, off);
    }
    return 0;
}

static bool blk_raw_trim(rvvm_blk_dev_t* blk, uint64_t off, uint64_t size)
{
    rvfile_t* file = rvvm_blk_get_data(blk);
    return rvtrim(file, off, size);
}

static const rvvm_blk_cb_t blk_raw_cb = {
    .name     = "raw",
    .probe    = blk_raw_probe,
    .close    = blk_raw_close,
    .get_size = blk_raw_get_size,
    .set_size = blk_raw_set_size,
    .read     = blk_raw_read,
    .write    = blk_raw_write,
    .trim     = blk_raw_trim,
};

/*
 * Block device API
 */

struct rvvm_blk_dev {
    // Block device image private data
    void* data;

    // Block device image callbacks
    const rvvm_blk_cb_t* cb;

    // Block device image size
    uint64_t size;

    // Drive head position
    uint64_t head;

    // Open options
    uint32_t opts;
};

RVVM_PUBLIC rvvm_blk_dev_t* rvvm_blk_open(const char* name, const char* fmt, uint32_t opts)
{
    rvvm_blk_dev_t* blk = rvvm_blk_init();
    if ((!fmt && rvvm_blk_vhd_detect(name)) || (fmt && rvvm_strcmp(fmt, rvvm_blk_vhd_cb.name))) {
        rvvm_blk_register_cb(blk, &rvvm_blk_vhd_cb, opts);
    } else if (!fmt || rvvm_strcmp(fmt, blk_raw_cb.name)) {
        rvvm_blk_register_cb(blk, &blk_raw_cb, opts);
    }
    if (blk->cb && blk->cb->probe && blk->cb->probe(blk, name, blk->opts)) {
        if (blk->cb->get_size) {
            blk->size = blk->cb->get_size(blk);
        }
        return blk;
    }
    rvvm_blk_close(blk);
    return NULL;
}

RVVM_PUBLIC void rvvm_blk_close(rvvm_blk_dev_t* blk)
{
    if (blk) {
        if (blk->cb && blk->cb->close) {
            blk->cb->close(blk);
        }
        safe_free(blk);
    }
}

RVVM_PUBLIC const char* rvvm_blk_get_format(rvvm_blk_dev_t* blk)
{
    if (blk) {
        return blk->cb->name;
    }
    return NULL;
}

RVVM_PUBLIC uint64_t rvvm_blk_get_size(rvvm_blk_dev_t* blk)
{
    if (blk) {
        return atomic_load_uint64_relax(&blk->size);
    }
    return 0;
}

RVVM_PUBLIC bool rvvm_blk_set_size(rvvm_blk_dev_t* blk, uint64_t size)
{
    if (likely(blk && (blk->opts & RVVM_BLK_WRITE))) {
        if (blk->cb->set_size && !blk->cb->set_size(blk, size)) {
            return false;
        }
        atomic_store_uint64(&blk->size, size);
        return true;
    }
    return false;
}

RVVM_PUBLIC size_t rvvm_blk_read(rvvm_blk_dev_t* blk, void* dst, size_t size, uint64_t off)
{
    if (likely(blk && (blk->opts & RVVM_BLK_READ))) {
        uint64_t blk_size = atomic_load_uint64_relax(&blk->size);
        if (likely(off < blk_size)) {
            uint64_t remain = blk_size - off;
            if (size > remain) {
                size = remain;
            }
            return blk->cb->read(blk, dst, size, off);
        }
    }
    return 0;
}

static void rvvm_blk_grow_size(rvvm_blk_dev_t* blk, uint64_t old_size, uint64_t size)
{
    do {
        if (size <= old_size) {
            return;
        }
    } while (!atomic_cas_uint64_loop(&blk->size, &old_size, size));
}

RVVM_PUBLIC size_t rvvm_blk_write(rvvm_blk_dev_t* blk, const void* src, size_t size, uint64_t off)
{
    if (likely(blk && (blk->opts & RVVM_BLK_WRITE))) {
        uint64_t blk_size = atomic_load_uint64_relax(&blk->size);
        if (likely(off < blk_size)) {
            uint64_t remain = blk_size - off;
            if (likely(size <= remain)) {
                return blk->cb->write(blk, src, size, off);
            } else if (!(blk->opts & RVVM_BLK_GROW)) {
                return blk->cb->write(blk, src, remain, off);
            }
        }
        if ((blk->opts & RVVM_BLK_GROW) && off + size > off) {
            size_t ret = blk->cb->write(blk, src, size, off);
            rvvm_blk_grow_size(blk, blk_size, off + ret);
            return ret;
        }
    }
    return 0;
}

RVVM_PUBLIC bool rvvm_blk_sync(rvvm_blk_dev_t* blk)
{
    if (likely(blk && blk->cb->sync && (blk->opts & RVVM_BLK_WRITE))) {
        return blk->cb->sync(blk);
    }
    return !!blk;
}

RVVM_PUBLIC bool rvvm_blk_trim(rvvm_blk_dev_t* blk, uint64_t off, uint64_t size)
{
    if (likely(blk && blk->cb->trim && (blk->opts & RVVM_BLK_WRITE))) {
        uint64_t blk_size = atomic_load_uint64_relax(&blk->size);
        if (likely(off < blk_size && size <= blk_size - off)) {
            return blk->cb->trim(blk, off, size);
        }
    }
    return false;
}

RVVM_PUBLIC bool rvvm_blk_seek_head(rvvm_blk_dev_t* blk, int64_t off, uint32_t whence)
{
    if (blk) {
        if (whence == RVVM_BLK_SEEK_CUR) {
            off = rvvm_blk_tell_head(blk) + off;
        } else if (whence == RVVM_BLK_SEEK_END) {
            off = atomic_load_uint64_relax(&blk->size) + off;
        } else if (whence != RVVM_BLK_SEEK_SET) {
            // Invalid seek operation
            off = -1;
        }
        if (off >= 0) {
            atomic_store_uint64_relax(&blk->head, off);
            return true;
        }
    }
    return false;
}

RVVM_PUBLIC uint64_t rvvm_blk_tell_head(rvvm_blk_dev_t* blk)
{
    if (blk) {
        return atomic_load_uint64_relax(&blk->head);
    }
    return 0;
}

RVVM_PUBLIC size_t rvvm_blk_write_head(rvvm_blk_dev_t* blk, const void* src, size_t size)
{
    size_t ret = rvvm_blk_write(blk, src, size, rvvm_blk_tell_head(blk));
    if (ret) {
        rvvm_blk_seek_head(blk, ret, RVVM_BLK_SEEK_CUR);
    }
    return ret;
}

RVVM_PUBLIC size_t rvvm_blk_read_head(rvvm_blk_dev_t* blk, void* dst, size_t size)
{
    size_t ret = rvvm_blk_read(blk, dst, size, rvvm_blk_tell_head(blk));
    if (ret) {
        rvvm_blk_seek_head(blk, ret, RVVM_BLK_SEEK_CUR);
    }
    return ret;
}

/*
 * Block device image format handling
 */

rvvm_blk_dev_t* rvvm_blk_init(void)
{
    return safe_new_obj(rvvm_blk_dev_t);
}

void rvvm_blk_register_cb(rvvm_blk_dev_t* blk, const rvvm_blk_cb_t* cb, uint32_t opts)
{
    if (blk && cb) {
        blk->cb   = cb;
        blk->opts = opts ? opts : (RVVM_BLK_RW | RVVM_BLK_DIRECT);
        if (!cb->read) {
            blk->opts &= ~RVVM_BLK_READ;
        }
        if (!cb->write) {
            blk->opts &= ~RVVM_BLK_WRITE;
        }
    }
}

void rvvm_blk_set_data(rvvm_blk_dev_t* blk, void* data)
{
    if (blk) {
        blk->data = data;
    }
}

void* rvvm_blk_get_data(rvvm_blk_dev_t* blk)
{
    if (blk) {
        return blk->data;
    }
    return NULL;
}

POP_OPTIMIZATION_SIZE

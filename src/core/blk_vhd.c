/*
blk_vhd.c - Fixed and dynamically expanding VHD block images
SPDX-License-Identifier: MPL-2.0

Format: https://www.microsoft.com/en-us/download/details.aspx?id=23850
Metadata is big-endian; each bitmap bit describes a 512-byte guest sector.
Differencing disks require a parent chain and are deliberately rejected.
*/

#include "blk_vhd.h"

#include <util/blk_io.h>
#include <util/locking.h>
#include <util/mem_ops.h>
#include <util/utils.h>

typedef struct {
    rvfile_t*   file;
    rvvm_lock_t lock;
    uint64_t    size;
    uint64_t    end;
    uint64_t    table;
    uint32_t    block_size;
    uint32_t    bitmap_size;
    uint32_t    cached_block;
    uint32_t*   bat;
    uint8_t*    bitmap;
    uint8_t     footer[512];
    bool        failed;
} blk_vhd_t;

bool rvvm_blk_vhd_detect(const char* path)
{
    rvfile_t* file = rvopen(path, RVFILE_READ);
    uint8_t   cookie[8];
    bool      found = false;
    if (file) {
        uint64_t size = rvfilesize(file);
        found         = rvread(file, cookie, sizeof(cookie), 0) == sizeof(cookie)
                     && (!memcmp(cookie, "conectix", 8) || !memcmp(cookie, "vhdxfile", 8));
        if (!found && size >= 512) {
            found
                = rvread(file, cookie, sizeof(cookie), size - 512) == sizeof(cookie) && !memcmp(cookie, "conectix", 8);
        }
        rvclose(file);
    }
    return found;
}

static bool vhd_checksum(const uint8_t* data, size_t size, size_t checksum)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < size; ++i) {
        if (i < checksum || i >= checksum + 4) {
            sum += data[i];
        }
    }
    return read_uint32_be_m(data + checksum) == ~sum;
}

static void blk_vhd_close(rvvm_blk_dev_t* blk)
{
    blk_vhd_t* vhd = rvvm_blk_get_data(blk);
    if (vhd) {
        rvclose(vhd->file);
        free(vhd->bat);
        free(vhd->bitmap);
        free(vhd);
    }
}

static int vhd_compare_block(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

static bool vhd_read_header(blk_vhd_t* vhd)
{
    uint8_t  header[1024];
    uint64_t offset = read_uint64_be_m(vhd->footer + 16);
    if (offset < 512 || (offset & 511) || offset > vhd->end || vhd->end - offset < sizeof(header)
        || rvread(vhd->file, header, sizeof(header), offset) != sizeof(header) || memcmp(header, "cxsparse", 8)
        || !vhd_checksum(header, sizeof(header), 36) || read_uint32_be_m(header + 24) != 0x00010000
        || read_uint64_be_m(header + 8) != UINT64_MAX) {
        return false;
    }
    vhd->table       = read_uint64_be_m(header + 16);
    vhd->block_size  = read_uint32_be_m(header + 32);
    uint32_t entries = read_uint32_be_m(header + 28);
    // Bound metadata allocations even for untrusted/corrupt images.
    if (vhd->block_size < 512 || vhd->block_size > 0x10000000 || (vhd->block_size & (vhd->block_size - 1)) || !entries
        || entries > 0x1000000 || entries < (vhd->size + vhd->block_size - 1) / vhd->block_size) {
        return false;
    }
    uint64_t table_size = (((uint64_t)entries * 4) + 511) & ~511ULL;
    if ((vhd->table & 511) || vhd->table < offset + sizeof(header) || vhd->table > vhd->end
        || table_size > vhd->end - vhd->table) {
        return false;
    }
    vhd->bitmap_size  = ((vhd->block_size / 512 + 7) / 8 + 511) & ~511U;
    vhd->bat          = safe_new_arr(uint32_t, entries);
    vhd->bitmap       = safe_new_arr(uint8_t, vhd->bitmap_size);
    vhd->cached_block = UINT32_MAX;
    if (rvread(vhd->file, vhd->bat, (size_t)entries * 4, vhd->table) != (size_t)entries * 4) {
        return false;
    }
    for (uint32_t i = 0; i < entries; ++i) {
        vhd->bat[i] = read_uint32_be_m(&vhd->bat[i]);
    }
    // A guest write must never be able to reach metadata or another block.
    uint32_t* sorted = safe_new_arr(uint32_t, entries);
    memcpy(sorted, vhd->bat, (size_t)entries * 4);
    qsort(sorted, entries, sizeof(*sorted), vhd_compare_block);
    uint64_t previous_end = vhd->table + table_size;
    uint64_t extent       = (uint64_t)vhd->bitmap_size + vhd->block_size;
    bool     valid        = true;
    for (uint32_t i = 0; i < entries && sorted[i] != UINT32_MAX; ++i) {
        uint64_t start = (uint64_t)sorted[i] * 512;
        if (start < previous_end || start > vhd->end || extent > vhd->end - start) {
            valid = false;
            break;
        }
        previous_end = start + extent;
    }
    free(sorted);
    return valid;
}

static bool blk_vhd_probe(rvvm_blk_dev_t* blk, const char* name, uint32_t opts)
{
    if (opts & (RVVM_BLK_TRUNC | RVVM_BLK_GROW)) {
        rvvm_error("VHD resize/truncation is not supported: %s", name);
        return false;
    }
    blk_vhd_t* vhd = safe_new_obj(blk_vhd_t);
    rvvm_blk_set_data(blk, vhd);
    rvvm_lock_init(&vhd->lock);
    // Metadata IO is not sector/alignment constrained. Always allow metadata reads.
    uint32_t file_opts = RVFILE_READ | (opts & (RVFILE_WRITE | RVFILE_EXCL | RVFILE_SYNC));
    // Concurrent writers cannot safely maintain a shared BAT/bitmap cache.
    if (opts & RVVM_BLK_WRITE) {
        file_opts |= RVFILE_EXCL;
    }
    vhd->file = rvopen(name, file_opts);
    if (!vhd->file) {
        return false;
    }
    uint64_t size = rvfilesize(vhd->file);
    if (size < 512 || (size & 511)) {
        goto invalid;
    }
    vhd->end = size - 512;
    if (rvread(vhd->file, vhd->footer, 512, vhd->end) != 512 || memcmp(vhd->footer, "conectix", 8)
        || !vhd_checksum(vhd->footer, 512, 64) || read_uint32_be_m(vhd->footer + 12) != 0x00010000) {
        goto invalid;
    }
    vhd->size = read_uint64_be_m(vhd->footer + 48);
    if (!vhd->size || (vhd->size & 511) || vhd->size > (1ULL << 41)) {
        goto invalid;
    }
    switch (read_uint32_be_m(vhd->footer + 60)) {
        case 2:
            if (vhd->size != vhd->end || read_uint64_be_m(vhd->footer + 16) != UINT64_MAX) {
                goto invalid;
            }
            break;
        case 3:
            if (!vhd_read_header(vhd)) {
                goto invalid;
            }
            break;
        case 4:
            rvvm_error("Differencing VHD requires a parent image and is not supported: %s", name);
            return false;
        default:
            goto invalid;
    }
    return true;
invalid:
    rvvm_error("Invalid or unsupported VHD image: %s", name);
    return false;
}

static uint64_t blk_vhd_get_size(rvvm_blk_dev_t* blk)
{
    blk_vhd_t* vhd = rvvm_blk_get_data(blk);
    return vhd->size;
}

static bool blk_vhd_set_size(rvvm_blk_dev_t* blk, uint64_t size)
{
    return size == blk_vhd_get_size(blk);
}

static bool vhd_bitmap(blk_vhd_t* vhd, uint32_t block)
{
    if (vhd->cached_block != block) {
        vhd->cached_block = UINT32_MAX;
        if (rvread(vhd->file, vhd->bitmap, vhd->bitmap_size, (uint64_t)vhd->bat[block] * 512) != vhd->bitmap_size) {
            return false;
        }
        vhd->cached_block = block;
    }
    return true;
}

static bool vhd_sector_present(blk_vhd_t* vhd, uint32_t sector)
{
    return !!(vhd->bitmap[sector >> 3] & (0x80U >> (sector & 7)));
}

static size_t vhd_read_dynamic(blk_vhd_t* vhd, uint8_t* dst, size_t size, uint64_t off)
{
    size_t done = 0;
    while (done < size) {
        uint32_t block = off / vhd->block_size;
        uint32_t pos   = off % vhd->block_size;
        size_t   chunk = EVAL_MIN(size - done, vhd->block_size - pos);
        if (vhd->bat[block] == UINT32_MAX) {
            memset(dst + done, 0, chunk);
        } else {
            if (!vhd_bitmap(vhd, block)) {
                break;
            }
            // Coalesce sectors with the same presence bit into one host IO.
            bool     present = vhd_sector_present(vhd, pos / 512);
            uint32_t end     = (pos | 511U) + 1;
            while (end < pos + chunk && vhd_sector_present(vhd, end / 512) == present) {
                end += 512;
            }
            chunk = EVAL_MIN(chunk, end - pos);
            if (present) {
                uint64_t physical = (uint64_t)vhd->bat[block] * 512 + vhd->bitmap_size + pos;
                size_t   ret      = rvread(vhd->file, dst + done, chunk, physical);
                if (ret != chunk) {
                    done += ret;
                    break;
                }
            } else {
                memset(dst + done, 0, chunk);
            }
        }
        done += chunk;
        off  += chunk;
    }
    return done;
}

static size_t blk_vhd_read(rvvm_blk_dev_t* blk, void* dst, size_t size, uint64_t off)
{
    blk_vhd_t* vhd = rvvm_blk_get_data(blk);
    rvvm_lock_slow(&vhd->lock);
    size_t ret = vhd->block_size ? vhd_read_dynamic(vhd, dst, size, off) : rvread(vhd->file, dst, size, off);
    rvvm_unlock(&vhd->lock);
    return ret;
}

static bool vhd_write(blk_vhd_t* vhd, const void* data, size_t size, uint64_t off)
{
    if (rvwrite(vhd->file, data, size, off) != size) {
        vhd->failed = true;
    }
    return !vhd->failed;
}

static bool vhd_sync(blk_vhd_t* vhd)
{
    if (!rvfsync(vhd->file)) {
        vhd->failed = true;
    }
    return !vhd->failed;
}

static bool vhd_allocate(blk_vhd_t* vhd, uint32_t block)
{
    uint64_t start = vhd->end;
    uint64_t end   = start + vhd->bitmap_size + vhd->block_size;
    uint8_t  entry[4];
    if (end / 512 >= UINT32_MAX) {
        return false;
    }
    // Install the new footer before replacing the old footer with a bitmap.
    // Unwritten payload sectors remain sparse and are hidden by the bitmap.
    vhd->cached_block = UINT32_MAX;
    memset(vhd->bitmap, 0, vhd->bitmap_size);
    write_uint32_be_m(entry, start / 512);
    if (!vhd_write(vhd, vhd->footer, 512, end) || !vhd_sync(vhd)
        || !vhd_write(vhd, vhd->bitmap, vhd->bitmap_size, start) || !vhd_sync(vhd)
        || !vhd_write(vhd, entry, sizeof(entry), vhd->table + (uint64_t)block * 4) || !vhd_sync(vhd)) {
        return false;
    }
    vhd->bat[block]   = start / 512;
    vhd->end          = end;
    vhd->cached_block = block;
    return true;
}

static size_t vhd_write_dynamic(blk_vhd_t* vhd, const uint8_t* src, size_t size, uint64_t off)
{
    size_t done = 0;
    while (done < size) {
        uint32_t block = off / vhd->block_size;
        uint32_t pos   = off % vhd->block_size;
        size_t   chunk = EVAL_MIN(size - done, vhd->block_size - pos);
        if (vhd->bat[block] == UINT32_MAX && !vhd_allocate(vhd, block)) {
            break;
        }
        if (!vhd_bitmap(vhd, block)) {
            break;
        }
        uint64_t data  = (uint64_t)vhd->bat[block] * 512 + vhd->bitmap_size;
        uint32_t first = pos / 512, last = (pos + chunk - 1) / 512;
        uint8_t  zero[512] = {0};
        // Preserve logical zeroes outside partial writes to previously absent sectors.
        if ((pos & 511) && !vhd_sector_present(vhd, first)
            && !vhd_write(vhd, zero, sizeof(zero), data + (uint64_t)first * 512)) {
            break;
        }
        if (((pos + chunk) & 511) && !vhd_sector_present(vhd, last)
            && !vhd_write(vhd, zero, sizeof(zero), data + (uint64_t)last * 512)) {
            break;
        }
        if (!vhd_write(vhd, src + done, chunk, data + pos)) {
            break;
        }
        bool changed = false;
        for (uint32_t sector = first; sector <= last; ++sector) {
            changed                  |= !vhd_sector_present(vhd, sector);
            vhd->bitmap[sector >> 3] |= 0x80U >> (sector & 7);
        }
        // Make data durable before making newly written sectors visible on reopen.
        if (changed && (!vhd_sync(vhd) || !vhd_write(vhd, vhd->bitmap, vhd->bitmap_size, data - vhd->bitmap_size))) {
            vhd->cached_block = UINT32_MAX;
            break;
        }
        done += chunk;
        off  += chunk;
    }
    return done;
}

static size_t blk_vhd_write(rvvm_blk_dev_t* blk, const void* src, size_t size, uint64_t off)
{
    blk_vhd_t* vhd = rvvm_blk_get_data(blk);
    size_t     ret = 0;
    rvvm_lock_slow(&vhd->lock);
    if (!vhd->failed) {
        if (vhd->block_size) {
            ret = vhd_write_dynamic(vhd, src, size, off);
        } else if (vhd_write(vhd, src, size, off)) {
            ret = size;
        }
    }
    rvvm_unlock(&vhd->lock);
    return ret;
}

static bool blk_vhd_sync(rvvm_blk_dev_t* blk)
{
    blk_vhd_t* vhd = rvvm_blk_get_data(blk);
    rvvm_lock_slow(&vhd->lock);
    bool ret = vhd_sync(vhd);
    rvvm_unlock(&vhd->lock);
    return ret;
}

const rvvm_blk_cb_t rvvm_blk_vhd_cb = {
    .name     = "vhd",
    .probe    = blk_vhd_probe,
    .close    = blk_vhd_close,
    .get_size = blk_vhd_get_size,
    .set_size = blk_vhd_set_size,
    .read     = blk_vhd_read,
    .write    = blk_vhd_write,
    .sync     = blk_vhd_sync,
    // No TRIM: raw host offsets must never be applied to VHD metadata.
};

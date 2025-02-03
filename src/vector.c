/*
vector.c - Vector Container
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "vector.h"

PUSH_OPTIMIZATION_SIZE

typedef struct {
    void* data;
    uint64_t size;
    uint64_t count;
} vector_punned_t;

// Grow factor: 1.5 (Better memory reusage), initial capacity: 2
slow_path void vector_grow_internal(void* vec, uint64_t elem_size, uint64_t pos)
{
    safe_aliasing vector_punned_t* vector = vec;
    if (!vector->size) {
        // Allocate the vector buffer
        vector->size = 2;
        vector->data = safe_calloc(elem_size, vector->size);
    }
    if (pos >= vector->size) {
        uint64_t new_size = vector->size;
        while (pos >= new_size) {
            new_size += (new_size >> 1);
        }
        vector->data = safe_realloc(vector->data, new_size * elem_size);
        vector->size = new_size;
    }
}

static void vector_move_elem_internal(safe_aliasing vector_punned_t* vector, uint64_t elem_size, uint64_t pos, bool erase)
{
    uint64_t move_count = (vector->count - pos) * elem_size;
    void* move_from = ((uint8_t*)vector->data) + ((pos + (erase ? 1 : 0)) * elem_size);
    void* move_to = ((uint8_t*)vector->data) + ((pos + (erase ? 0 : 1)) * elem_size);
    memmove(move_to, move_from, move_count);
}

void vector_emplace_internal(void* vec, uint64_t elem_size, uint64_t pos)
{
    safe_aliasing vector_punned_t* vector = vec;
    if (pos < vector->count) {
        vector_move_elem_internal(vector, elem_size, pos, false);
        vector->count++;
    } else {
        uint64_t new_count = pos + 1;
        if (unlikely(pos >= vector->size)) {
            vector_grow_internal(vec, elem_size, pos);
        }
        void* old_end = ((uint8_t*)vector->data) + (vector->count * elem_size);
        memset(old_end, 0, (new_count - vector->count) * elem_size);
        vector->count = new_count;
    }
}

void vector_erase_internal(void* vec, uint64_t elem_size, uint64_t pos)
{
    safe_aliasing vector_punned_t* vector = vec;
    if (pos < vector->count) {
        vector->count--;
        vector_move_elem_internal(vector, elem_size, pos, true);
    }
}

POP_OPTIMIZATION_SIZE

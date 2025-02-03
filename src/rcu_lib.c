/*
rcu_lib.c - Read-copy-update (RCU)
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "rcu_lib.h"
#include "threading.h"
#include "vma_ops.h"
#include "spinlock.h"
#include "vector.h"

RCU_THREAD_LOCAL uint32_t rcu_internal_reader_state = 0;

uint32_t rcu_internal_writer_waiting = 0;

static cond_var_t* rcu_writer_cond = NULL;

#if !defined(RCU_DUMMY_SHARED_STATE)

uint32_t rcu_internal_writer_gen = 1;

uint32_t rcu_has_remote_barriers = 0;

static spinlock_t rcu_global_lock = {0};

static vector_t(uint32_t*) rcu_readers_list = {0};

static void rcu_global_membarrier(void)
{
    atomic_fence_ex(ATOMIC_SEQ_CST);
    if (atomic_load_uint32_ex(&rcu_has_remote_barriers, ATOMIC_RELAXED)) {
        if (!vma_broadcast_membarrier()) {
            rvvm_fatal("vma_broadcast_membarrier() failed!");
        }
    }
}

static void rcu_global_init_once(void)
{
    rcu_writer_cond = condvar_create();
    if (rvvm_has_arg("rcu_nomembarrier")) {
        return;
    }
    if (vma_broadcast_membarrier()) {
        rvvm_info("[rcu] Remote barriers available");
        atomic_store_uint32(&rcu_has_remote_barriers, 1);
    } else {
        rvvm_info("[rcu] Falling back to reader barriers");
    }
}

static void rcu_next_generation(void)
{
    // Flip the RCU writer generation
    atomic_xor_uint32(&rcu_internal_writer_gen, RCU_INTERNAL_STATE_GEN);
    while (true) {
        uint32_t gen = atomic_load_uint32(&rcu_internal_writer_gen);
        bool wait = false;

        // Check every registered reader thread
        vector_foreach(rcu_readers_list, j) {
            uint32_t state = atomic_load_uint32(vector_at(rcu_readers_list, j));
            if (RCU_INTERNAL_STATE_READERS(state) && ((state ^ gen) & RCU_INTERNAL_STATE_GEN)) {
                // There are readers inside previous generation, keep waiting
                wait = true;
                break;
            }
        }

        if (wait) {
            atomic_store_uint32(&rcu_internal_writer_waiting, true);
            condvar_wait(rcu_writer_cond, 10);
            atomic_store_uint32(&rcu_internal_writer_waiting, false);
        } else {
            // Quiescent period
            return;
        }
    }
}

#endif

slow_path void rcu_internal_wake_writer(uint32_t state)
{
    if (!RCU_INTERNAL_STATE_READERS(state)) {
        rvvm_fatal("Mismatched rcu_read_unlock()!");
    } else if (RCU_INTERNAL_STATE_READERS(state) == 1) {
        condvar_wake(rcu_writer_cond);
    }
}

void rcu_register_thread(void)
{
#if !defined(RCU_DUMMY_SHARED_STATE)
    uint32_t* self_state = &rcu_internal_reader_state;
    DO_ONCE(rcu_global_init_once());
    rcu_deregister_thread();
    spin_lock_slow(&rcu_global_lock);
    vector_push_back(rcu_readers_list, self_state);
    spin_unlock(&rcu_global_lock);
#endif
}

void rcu_deregister_thread(void)
{
#if !defined(RCU_DUMMY_SHARED_STATE)
    uint32_t* self_state = &rcu_internal_reader_state;
    spin_lock_slow(&rcu_global_lock);
    vector_foreach_back(rcu_readers_list, i) {
        if (vector_at(rcu_readers_list, i) == self_state) {
            vector_erase(rcu_readers_list, i);
            self_state = NULL;
            break;
        }
    }
    spin_unlock(&rcu_global_lock);
#endif
}

void rcu_synchronize(void)
{
#if defined(RCU_DUMMY_SHARED_STATE)
    // Dummy shared reader implementation: Simply wait until all readers exit
    while (atomic_load_uint32(&rcu_internal_reader_state)) {
        atomic_store_uint32(&rcu_internal_writer_waiting, true);
        condvar_wait(rcu_writer_cond, 10);
        atomic_store_uint32(&rcu_internal_writer_waiting, false);
    }
#else
    // Perform StoreLoad barrier remotely on behalf of rcu_read_lock()
    rcu_global_membarrier();

    spin_lock_slow(&rcu_global_lock);

    /*
     * Bump the writer generation twice
     *
     * Why twice? The reader thread may have fetched rcu_internal_writer_gen,
     * but not yet stored it into rcu_internal_reader_state.
     */
    rcu_next_generation();
    rcu_next_generation();

    spin_unlock(&rcu_global_lock);

    // Perform LoadStore barrier remotely on behalf of rcu_read_unlock()
    rcu_global_membarrier();
#endif
}

#ifdef USE_INFRASTRUCTURE_TESTS

#include "utils.h"
#include "rvtimer.h"

/*
 * RCU Torture test & benchmark
 */

#define RCU_TORTURE_OBJSIZE 4

static uint8_t* rcu_torture_ptr = NULL;

static uint64_t rcu_torture_read_ops = 0;
static uint64_t rcu_torture_grace_periods = 0;

static void* rcu_torture_read_worker(void* arg)
{
    bool running = true;
    rcu_register_thread();
    while (running) {
        for (uint64_t iter = 0; iter < 4096; ++iter) {
            rcu_read_lock();
            uint8_t* ptr = rcu_dereference(rcu_torture_ptr);
            if (ptr) {
                for (uint64_t i = 0; i < RCU_TORTURE_OBJSIZE; ++i) {
                    if (ptr[i] != i + 1) {
                        rvvm_fatal("RCU torture test detected a datarace!!!");
                    }
                }
            } else {
                running = false;
            }
            rcu_read_unlock();
        }

        atomic_add_uint64_ex(&rcu_torture_read_ops, 4096, ATOMIC_RELAXED);
    }
    rcu_deregister_thread();
    return arg;
}

static uint8_t* rcu_torture_new_ptr(void)
{
    uint8_t* ptr = safe_new_arr(uint8_t, RCU_TORTURE_OBJSIZE);
    for (uint64_t i = 0; i < RCU_TORTURE_OBJSIZE; ++i) {
        ptr[i] = i + 1;
    }
    return ptr;
}

static void* rcu_torture_write_worker(void* arg)
{
    while (true) {
        uint8_t* old_ptr = rcu_swap_pointer(rcu_torture_ptr, rcu_torture_new_ptr());

        rcu_synchronize();

        for (uint64_t i = 0; i < RCU_TORTURE_OBJSIZE; ++i) {
            old_ptr[i] = 0;
        }

        free(old_ptr);

        if (rvvm_has_arg("rcu_sloppy_writer")) {
            sleep_ms(100);
        }

        atomic_add_uint64_ex(&rcu_torture_grace_periods, 1, ATOMIC_RELAXED);
    }
    return arg;
}

void rcu_torture_test(uint32_t reader_threads, uint32_t secs)
{
    rcu_torture_ptr = rcu_torture_new_ptr();
    if (!reader_threads) {
        reader_threads = 4;
    }
    if (!secs) {
        secs = 60;
    }

    for (uint32_t i = 0; i < reader_threads; ++i) {
        thread_create(rcu_torture_read_worker, NULL);
    }

    thread_create(rcu_torture_write_worker, NULL);

    for (uint32_t i = 0; i < secs; ++i) {
        sleep_ms(1000);
        uint64_t reads = atomic_swap_uint64(&rcu_torture_read_ops, 0);
        uint64_t grace_periods = atomic_swap_uint64(&rcu_torture_grace_periods, 0);
        rvvm_warn("rcu reads: %"PRIu64"/s, grace periods: %"PRIu64"/s", reads, grace_periods);
    }

    rvvm_warn("RCU torture test passed!");
}

#endif

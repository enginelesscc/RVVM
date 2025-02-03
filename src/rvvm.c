/*
rvvm.c - RISC-V Virtual Machine
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "rvvm.h"
#include "rvvm_isolation.h"
#include "riscv_hart.h"
#include "riscv_mmu.h"
#include "riscv_cpu.h"
#include "vector.h"
#include "utils.h"
#include "mem_ops.h"
#include "threading.h"
#include "spinlock.h"
#include "elf_load.h"
#include "stacktrace.h"

static spinlock_t global_lock = SPINLOCK_INIT;
static vector_t(rvvm_machine_t*) global_machines = {0};
static bool global_manual = false;

static spinlock_t eventloop_lock = SPINLOCK_INIT;
static cond_var_t* eventloop_cond = NULL;
static thread_ctx_t* eventloop_thread = NULL;

static inline char* rvvm_merge_strings_internal(const char* str1, const char* str2)
{
    uint64_t str1_len = str1 ? rvvm_strlen(str1) : 0;
    uint64_t str2_len = str2 ? rvvm_strlen(str2) : 0;
    char* ret = safe_new_arr(char, str1_len + str2_len + 1);
    memcpy(ret, str1, str1_len);
    memcpy(ret + str1_len, str2, str2_len);
    return ret;
}

void rvvm_append_isa_string(rvvm_machine_t* machine, const char* str)
{
#ifdef USE_FDT
    vector_foreach(machine->harts, i) {
        struct fdt_node* cpus = fdt_node_find(rvvm_get_fdt_root(machine), "cpus");
        struct fdt_node* cpu = fdt_node_find_reg(cpus, "cpu", i);
        char* isa = fdt_node_get_prop_data(cpu, "riscv,isa");
        char* new = rvvm_merge_strings_internal(isa, str);
        fdt_node_add_prop_str(cpu, "riscv,isa", new);
        free(new);
    }
#else
    UNUSED(machine);
    UNUSED(str);
#endif
}

#ifdef USE_FDT

static void rvvm_init_fdt(rvvm_machine_t* machine)
{
    machine->fdt = fdt_node_create(NULL);
    fdt_node_add_prop_u32(machine->fdt, "#address-cells", 2);
    fdt_node_add_prop_u32(machine->fdt, "#size-cells", 2);
    fdt_node_add_prop_str(machine->fdt, "model", "RVVM v"RVVM_VERSION);
    fdt_node_add_prop(machine->fdt, "compatible", "lekkit,rvvm\0riscv-virtio\0", 25);

    // FDT /chosen node
    struct fdt_node* chosen = fdt_node_create("chosen");
    uint8_t rng_buffer[64] = {0};
    rvvm_randombytes(rng_buffer, sizeof(rng_buffer));
    fdt_node_add_prop(chosen, "rng-seed", rng_buffer, sizeof(rng_buffer));
    fdt_node_add_child(machine->fdt, chosen);

    // FDT /memory node
    struct fdt_node* memory = fdt_node_create_reg("memory", machine->mem.addr);
    fdt_node_add_prop_str(memory, "device_type", "memory");
    fdt_node_add_prop_reg(memory, "reg", machine->mem.addr, machine->mem.size);
    fdt_node_add_child(machine->fdt, memory);

    // FDT /cpus node
    struct fdt_node* cpus = fdt_node_create("cpus");
    fdt_node_add_prop_u32(cpus, "#address-cells", 1);
    fdt_node_add_prop_u32(cpus, "#size-cells", 0);
    fdt_node_add_prop_u32(cpus, "timebase-frequency", rvvm_get_opt(machine, RVVM_OPT_TIME_FREQ));
    fdt_node_add_child(machine->fdt, cpus);

    struct fdt_node* cpu_map = fdt_node_create("cpu-map");
    struct fdt_node* cluster = fdt_node_create("cluster0");
    fdt_node_add_child(cpu_map, cluster);

    vector_foreach(machine->harts, i) {
        struct fdt_node* cpu = fdt_node_create_reg("cpu", i);

        fdt_node_add_prop_str(cpu, "device_type", "cpu");
        fdt_node_add_prop_u32(cpu, "reg", i);
        fdt_node_add_prop(cpu, "compatible", "lekkit,rvvm\0riscv\0", 18);
        fdt_node_add_prop_u32(cpu, "clock-frequency", 3000000000);
        fdt_node_add_prop_u32(cpu, "riscv,cboz-block-size", 64);
        fdt_node_add_prop_u32(cpu, "riscv,cbom-block-size", 64);
        if (vector_at(machine->harts, i)->rv64) {
#ifdef USE_FPU
            fdt_node_add_prop_str(cpu, "riscv,isa", "rv64imafdcb_zicsr_zifencei_zkr_zicboz_zicbom_svadu_sstc");
#else
            fdt_node_add_prop_str(cpu, "riscv,isa", "rv64imacb_zicsr_zifencei_zkr_zicboz_zicbom_svadu_sstc");
#endif
            fdt_node_add_prop_str(cpu, "mmu-type", "riscv,sv39");
        } else {
#ifdef USE_FPU
            fdt_node_add_prop_str(cpu, "riscv,isa", "rv32imafdcb_zicsr_zifencei_zkr_zicboz_zicbom_svadu_sstc");
#else
            fdt_node_add_prop_str(cpu, "riscv,isa", "rv32imacb_zicsr_zifencei_zkr_zicboz_zicbom_svadu_sstc");
#endif
            fdt_node_add_prop_str(cpu, "mmu-type", "riscv,sv32");
        }

        fdt_node_add_prop_str(cpu, "status", "okay");

        struct fdt_node* clic = fdt_node_create("interrupt-controller");
        fdt_node_add_prop_u32(clic, "#interrupt-cells", 1);
        fdt_node_add_prop(clic, "interrupt-controller", NULL, 0);
        fdt_node_add_prop_str(clic, "compatible", "riscv,cpu-intc");
        fdt_node_add_child(cpu, clic);

        fdt_node_add_child(cpus, cpu);

        char core_name[32] = "core";
        int_to_str_dec(core_name + 4, 20, i);
        struct fdt_node* core = fdt_node_create(core_name);
        fdt_node_add_prop_u32(core, "cpu", fdt_node_get_phandle(cpu));
        fdt_node_add_child(cluster, core);
    }

    fdt_node_add_child(cpus, cpu_map);

    // FDT /soc node
    struct fdt_node* soc = fdt_node_create("soc");
    fdt_node_add_prop_u32(soc, "#address-cells", 2);
    fdt_node_add_prop_u32(soc, "#size-cells", 2);
    fdt_node_add_prop_str(soc, "compatible", "simple-bus");
    fdt_node_add_prop(soc, "ranges", NULL, 0);
    fdt_node_add_child(machine->fdt, soc);
    machine->fdt_soc = soc;
}

static void rvvm_prepare_fdt(rvvm_machine_t* machine)
{
    if (rvvm_get_opt(machine, RVVM_OPT_HW_IMITATE)) {
        fdt_node_add_prop_str(machine->fdt, "model", "PlasmaArc Five");
        fdt_node_add_prop_str(machine->fdt, "compatible", "lekkit,arc7xx");

        vector_foreach(machine->harts, i) {
            struct fdt_node* cpus = fdt_node_find(machine->fdt, "cpus");
            struct fdt_node* cpu = fdt_node_find_reg(cpus, "cpu", i);
            fdt_node_add_prop(cpu, "compatible", "lekkit,arc7xx\0riscv\0", 20);
        }
    }
}

#endif

#define RVVM_POWER_OFF   0
#define RVVM_POWER_ON    1
#define RVVM_POWER_RESET 2

static uint64_t rvvm_dtb_addr(rvvm_machine_t* machine, uint64_t dtb_size)
{
    return align_size_down(machine->mem.size > dtb_size ? machine->mem.size - dtb_size : 0, 8);
}

static rvvm_addr_t rvvm_pass_dtb(rvvm_machine_t* machine)
{
    if (rvvm_get_opt(machine, RVVM_OPT_DTB_ADDR)) {
        // API user manually passes DTB
        return rvvm_get_opt(machine, RVVM_OPT_DTB_ADDR);
    } else if (machine->dtb_file) {
        // Load DTB from file
        uint32_t dtb_size = rvfilesize(machine->dtb_file);
        uint64_t dtb_off = rvvm_dtb_addr(machine, dtb_size);
        if (dtb_size < machine->mem.size) {
            rvread(machine->dtb_file, ((uint8_t*)machine->mem.data) + dtb_off, machine->mem.size - dtb_off, 0);
            rvvm_info("Loaded DTB at 0x%08"PRIx64", size %u", machine->mem.addr + dtb_off, dtb_size);
            return machine->mem.addr + dtb_off;
        }
    } else {
        // Generate DTB
#ifdef USE_FDT
        rvvm_prepare_fdt(machine);
        uint32_t dtb_size = fdt_size(machine->fdt);
        uint64_t dtb_off = rvvm_dtb_addr(machine, dtb_size);
        if (fdt_serialize(machine->fdt, ((uint8_t*)machine->mem.data) + dtb_off, machine->mem.size - dtb_off, 0)) {
            rvvm_info("Generated DTB at 0x%08"PRIx64", size %u", machine->mem.addr + dtb_off, dtb_size);
            return machine->mem.addr + dtb_off;
        }
#else
        rvvm_error("This build doesn't support FDT generation");
        return 0;
#endif
    }

    rvvm_error("Device tree does not fit in RAM!");
    return 0;
}

static void rvvm_reset_machine_state(rvvm_machine_t* machine)
{
    atomic_store_uint32(&machine->power_state, RVVM_POWER_ON);

    // Reset devices
    vector_foreach(machine->mmio_devs, i) {
        rvvm_mmio_dev_t* dev = vector_at(machine->mmio_devs, i);
        if (dev->type && dev->type->reset) dev->type->reset(dev);
    }
    // Load bootrom, kernel, dtb into RAM if needed
    bool elf = !rvvm_get_opt(machine, RVVM_OPT_HW_IMITATE);
    if (machine->bootrom_file) {
        bin_objcopy(machine->bootrom_file, machine->mem.data, machine->mem.size, elf);
    }
    if (machine->kernel_file) {
        uint64_t kernel_offset = machine->rv64 ? 0x200000 : 0x400000;
        uint64_t kernel_size = machine->mem.size > kernel_offset ? machine->mem.size - kernel_offset : 0;
        bin_objcopy(machine->kernel_file, ((uint8_t*)machine->mem.data) + kernel_offset, kernel_size, elf);
    }
    rvvm_addr_t dtb_addr = rvvm_pass_dtb(machine);
    // Reset CPUs
    rvtimer_init(&machine->timer, rvvm_get_opt(machine, RVVM_OPT_TIME_FREQ));
    vector_foreach(machine->harts, i) {
        rvvm_hart_t* vm = vector_at(machine->harts, i);
        // a0 register & mhartid csr contain hart ID
        vm->csr.hartid = i;
        vm->registers[RISCV_REG_X10] = i;
        // a1 register contains FDT address
        vm->registers[RISCV_REG_X11] = dtb_addr;
        // Jump to RESET_PC
        vm->registers[RISCV_REG_PC] = rvvm_get_opt(machine, RVVM_OPT_RESET_PC);
        riscv_switch_priv(vm, RISCV_PRIV_MACHINE);
        riscv_jit_flush_cache(vm);
    }
}

static bool rvvm_eventloop_tick(bool manual)
{
    bool ret = false;
    if (vector_size(global_machines) == 0 || global_manual == !manual) {
        // No running machines left or switched eventloop mode
        return true;
    }

    vector_foreach_back(global_machines, m) {
        rvvm_machine_t* machine = vector_at(global_machines, m);
        uint32_t power_state = atomic_load_uint32(&machine->power_state);

        if (power_state == RVVM_POWER_ON) {
            vector_foreach(machine->harts, i) {
                rvvm_hart_t* vm = vector_at(machine->harts, i);
                // Сheck hart timer interrupts
                riscv_hart_check_timer(vector_at(machine->harts, i));
                if (rvvm_get_opt(machine, RVVM_OPT_MAX_CPU_CENT) < 100) {
                    uint32_t preempt = 10 - ((10 * rvvm_get_opt(machine, RVVM_OPT_MAX_CPU_CENT) + 9) / 100);
                    riscv_hart_preempt(vm, preempt);
                }
            }

            vector_foreach(machine->mmio_devs, i) {
                rvvm_mmio_dev_t* dev = vector_at(machine->mmio_devs, i);
                if (dev->type && dev->type->update) {
                    // Update device
                    dev->type->update(dev);
                }
            }
        } else {
            // The machine was shut down or reset
            vector_foreach(machine->harts, i) {
                riscv_hart_pause(vector_at(machine->harts, i));
            }
            // Call reset/poweroff handler
            if (power_state == RVVM_POWER_RESET) {
                rvvm_info("Machine %p resetting", machine);
                rvvm_reset_machine_state(machine);
                vector_foreach(machine->harts, i) {
                    riscv_hart_spawn(vector_at(machine->harts, i));
                }
            } else {
                rvvm_info("Machine %p shutting down", machine);
                atomic_store_uint32(&machine->running, false);
                vector_erase(global_machines, m);
                if (manual) {
                    // Return from manual eventloop whenever a machine powers down
                    ret = true;
                }
            }
        }
    }
    return ret;
}

#ifdef __EMSCRIPTEN__

// Implement proper Emscripten eventloop instead of a built-in one
#include <emscripten.h>

static void rvvm_eventloop_tick_em(void)
{
    rvvm_eventloop_tick(true);
}

#endif

static void* rvvm_eventloop(void* manual)
{
#ifdef __EMSCRIPTEN__
    if (manual) {
        emscripten_set_main_loop(rvvm_eventloop_tick_em, 0, true);
    }
#else
    if (!manual && !rvvm_has_arg("noisolation")) {
        rvvm_restrict_this_thread();
    }

    while (true) {
        spin_lock(&global_lock);
        if (rvvm_eventloop_tick(!!manual)) {
            spin_unlock(&global_lock);
            break;
        }
        spin_unlock(&global_lock);
        condvar_wait(eventloop_cond, 16);
    }
#endif
    return NULL;
}

static void rvvm_reconfigure_eventloop(void)
{
#ifdef __EMSCRIPTEN__
    spin_lock(&eventloop_lock);
    eventloop_thread = NULL;
    emscripten_cancel_main_loop();
    if (!global_manual) {
        emscripten_set_main_loop(rvvm_eventloop_tick_em, 0, false);
    }
    spin_unlock(&eventloop_lock);
#else
    spin_lock(&global_lock);
    bool needs_cond = global_manual || vector_size(global_machines);
    bool needs_thread = !global_manual && vector_size(global_machines);
    spin_unlock(&global_lock);

    spin_lock(&eventloop_lock);
    if (!needs_thread && eventloop_thread) {
        condvar_wake(eventloop_cond);
        thread_join(eventloop_thread);
        eventloop_thread = NULL;
    }

    if (!needs_cond && eventloop_cond) {
        condvar_free(eventloop_cond);
        eventloop_cond = NULL;
    }

    if (needs_cond && !eventloop_cond) {
        eventloop_cond = condvar_create();
    }

    if (needs_thread && !eventloop_thread) {
        eventloop_thread = thread_create(rvvm_eventloop, NULL);
    }
    spin_unlock(&eventloop_lock);
#endif
}

static void rvvm_set_manual_eventloop(bool manual)
{
    spin_lock(&global_lock);
    global_manual = manual;
    spin_unlock(&global_lock);
    rvvm_reconfigure_eventloop();
}

static bool rvvm_reopen_check_size(rvfile_t** dest, const char* path, uint64_t size)
{
    rvclose(*dest);
    if (path) {
        *dest = rvopen(path, 0);
        if (*dest == NULL) {
            rvvm_error("Could not open file %s", path);
            return false;
        }
        if (rvfilesize(*dest) > size) {
            rvvm_error("File %s doesn't fit in RAM", path);
            rvclose(*dest);
            *dest = NULL;
            return false;
        }
    } else {
        *dest = NULL;
    }
    return true;
}

static void rvvm_mmio_free(rvvm_mmio_dev_t* dev)
{
    rvvm_info("Removing MMIO device \"%s\"", dev->type ? dev->type->name : "null");
    rvvm_cleanup_mmio_desc(dev);
    free(dev);
}

PUBLIC bool rvvm_check_abi(int abi)
{
    if (RVVM_ABI_VERSION < 0) {
        rvvm_warn("This is a staging librvvm build with unstable ABI/API");
        return true;
    }
    return abi == RVVM_ABI_VERSION;
}

/*
 * RVVM Machine Management
 */

PUBLIC rvvm_machine_t* rvvm_create_machine(uint64_t mem_size, uint64_t hart_count, const char* isa)
{
    stacktrace_init();
    // TODO: Proper full ISA string parsing
    if (!isa) isa = "rv64";
    bool rv64 = rvvm_strfind(isa, "rv64") == isa;
    bool rv32 = rvvm_strfind(isa, "rv32") == isa;
    if (!rv64 && !rv32) {
        rvvm_error("Invalid CPU ISA string: %s", isa);
        return NULL;
    }
#ifndef USE_RV32
    if (rv32) {
        rvvm_error("RV32 is disabled in this RVVM build");
        return NULL;
    }
#endif
#ifndef USE_RV64
    if (rv64) {
        rvvm_error("RV64 is disabled in this RVVM build");
        return NULL;
    }
#endif
    if (hart_count == 0 || hart_count > 1024) {
        rvvm_error("Invalid machine core count");
        return NULL;
    }
    if (rv32 && mem_size > (1U << 30)) {
        // Workaround for SBI/Linux hangs on too much ram
        rvvm_warn("Creating RV32 machine with >1G of RAM is likely to break, fixing");
        mem_size = 1U << 30;
    }

    rvvm_machine_t* machine = safe_new_obj(rvvm_machine_t);
    machine->rv64 = rv64;
    if (!riscv_init_ram(&machine->mem, RVVM_DEFAULT_MEMBASE, mem_size)) {
        free(machine);
        return NULL;
    }

    // Default options
    rvvm_set_opt(machine, RVVM_OPT_MAX_CPU_CENT, 100);
    rvvm_set_opt(machine, RVVM_OPT_MEM_BASE, RVVM_DEFAULT_MEMBASE);
    rvvm_set_opt(machine, RVVM_OPT_RESET_PC, RVVM_DEFAULT_MEMBASE);
    rvvm_set_opt(machine, RVVM_OPT_TIME_FREQ, 10000000);

#ifdef USE_JIT
    rvvm_set_opt(machine, RVVM_OPT_JIT, !rvvm_has_arg("nojit"));
    rvvm_set_opt(machine, RVVM_OPT_JIT_HARVARD, rvvm_has_arg("rvjit_harvard"));
    if (rvvm_getarg_size("jitcache")) {
        rvvm_set_opt(machine, RVVM_OPT_JIT_CACHE, rvvm_getarg_size("jitcache"));
    } else {
        uint64_t jit_cache = 16 << 20;
        if (mem_size >= (512U << 20)) jit_cache = 32 << 20;
        if (mem_size >= (1U << 30))   jit_cache = 64 << 20;
        // Default 16M-64M JIT cache per hart (depends on RAM)
        rvvm_set_opt(machine, RVVM_OPT_JIT_CACHE, jit_cache);
    }
#endif

    for (uint64_t i=0; i<hart_count; ++i) {
        vector_push_back(machine->harts, riscv_hart_init(machine));
    }
#ifdef USE_FDT
    rvvm_init_fdt(machine);
#endif
    return machine;
}

PUBLIC void rvvm_set_cmdline(rvvm_machine_t* machine, const char* str)
{
#ifdef USE_FDT
    struct fdt_node* chosen = fdt_node_find(machine->fdt, "chosen");
    fdt_node_add_prop_str(chosen, "bootargs", str);
#else
    UNUSED(machine);
    UNUSED(str);
#endif
}

PUBLIC void rvvm_append_cmdline(rvvm_machine_t* machine, const char* str)
{
#ifdef USE_FDT
    struct fdt_node* chosen = fdt_node_find(machine->fdt, "chosen");
    char* cmdline = fdt_node_get_prop_data(chosen, "bootargs");
    char* tmp = cmdline ? rvvm_merge_strings_internal(cmdline, " ") : NULL;
    char* new = rvvm_merge_strings_internal(tmp, str);
    fdt_node_add_prop_str(chosen, "bootargs", new);
    free(tmp);
    free(new);
#else
    UNUSED(machine);
    UNUSED(str);
#endif
}

PUBLIC bool rvvm_load_bootrom(rvvm_machine_t* machine, const char* path)
{
    return rvvm_reopen_check_size(&machine->bootrom_file, path, machine->mem.size);
}

PUBLIC bool rvvm_load_kernel(rvvm_machine_t* machine, const char* path)
{
    uint64_t kernel_offset = machine->rv64 ? 0x200000 : 0x400000;
    uint64_t kernel_size = machine->mem.size > kernel_offset ? machine->mem.size - kernel_offset : 0;
    return rvvm_reopen_check_size(&machine->kernel_file, path, kernel_size);
}

PUBLIC bool rvvm_load_dtb(rvvm_machine_t* machine, const char* path)
{
    return rvvm_reopen_check_size(&machine->dtb_file, path, machine->mem.size >> 1);
}

PUBLIC bool rvvm_dump_dtb(rvvm_machine_t* machine, const char* path)
{
#ifdef USE_FDT
    rvfile_t* file = rvopen(path, RVFILE_RW | RVFILE_CREAT | RVFILE_TRUNC);
    if (file) {
        uint64_t size = fdt_size(rvvm_get_fdt_root(machine));
        void* buffer = safe_calloc(size, 1);
        size = fdt_serialize(rvvm_get_fdt_root(machine), buffer, size, 0);
        rvwrite(file, buffer, size, 0);
        rvclose(file);
        free(buffer);
        return true;
    }
#else
    UNUSED(machine);
    UNUSED(path);
    rvvm_error("This build doesn't support FDT generation");
#endif
    return false;
}

PUBLIC rvvm_addr_t rvvm_get_opt(rvvm_machine_t* machine, uint32_t opt)
{
    if (opt < RVVM_OPTS_ARR_SIZE) {
        return atomic_load_uint64_ex(&machine->opts[opt], ATOMIC_RELAXED);
    }
    switch (opt) {
        case RVVM_OPT_MEM_BASE: return machine->mem.addr;
        case RVVM_OPT_MEM_SIZE: return machine->mem.size;
        case RVVM_OPT_HART_COUNT: return vector_size(machine->harts);
    }
    return 0;
}

PUBLIC bool rvvm_set_opt(rvvm_machine_t* machine, uint32_t opt, rvvm_addr_t val)
{
    if (opt < RVVM_OPTS_ARR_SIZE) {
        atomic_store_uint64_ex(&machine->opts[opt], val, ATOMIC_RELAXED);
        return true;
    }
    switch (opt) {
        case RVVM_OPT_MEM_BASE:
            machine->mem.addr = val;
            return true;
    }
    return false;
}

PUBLIC bool rvvm_start_machine(rvvm_machine_t* machine)
{
    if (atomic_swap_uint32(&machine->running, true)) {
        return false;
    }

    spin_lock(&global_lock);

    if (!rvvm_machine_powered(machine)) {
        rvvm_reset_machine_state(machine);
    }

    vector_foreach(machine->harts, i) {
        riscv_hart_prepare(vector_at(machine->harts, i));
    }
    vector_foreach(machine->harts, i) {
        riscv_hart_spawn(vector_at(machine->harts, i));
    }

    // Register the machine as running
    vector_push_back(global_machines, machine);
    spin_unlock(&global_lock);

    rvvm_reconfigure_eventloop();
    return true;
}

PUBLIC bool rvvm_pause_machine(rvvm_machine_t* machine)
{
    if (!atomic_swap_uint32(&machine->running, false)) {
        return false;
    }

    spin_lock(&global_lock);

    vector_foreach(machine->harts, i) {
        riscv_hart_pause(vector_at(machine->harts, i));
    }

    vector_foreach_back(global_machines, i) {
        if (vector_at(global_machines, i) == machine) {
            vector_erase(global_machines, i);
            break;
        }
    }
    spin_unlock(&global_lock);

    rvvm_reconfigure_eventloop();
    return true;
}

PUBLIC void rvvm_reset_machine(rvvm_machine_t* machine, bool reset)
{
    // Handled by eventloop
    atomic_store_uint32(&machine->power_state, reset ? RVVM_POWER_RESET : RVVM_POWER_OFF);

    // For singlethreaded VMs, returns from riscv_hart_run()
    if (vector_size(machine->harts) == 1) {
        riscv_hart_queue_pause(vector_at(machine->harts, 0));
    }
    condvar_wake(eventloop_cond);
}

PUBLIC bool rvvm_machine_running(rvvm_machine_t* machine)
{
    return atomic_load_uint32(&machine->running);
}

PUBLIC bool rvvm_machine_powered(rvvm_machine_t* machine)
{
    return atomic_load_uint32(&machine->power_state) != RVVM_POWER_OFF;
}

PUBLIC void rvvm_free_machine(rvvm_machine_t* machine)
{
    rvvm_pause_machine(machine);

    // Shut down the eventloop if needed
    rvvm_reconfigure_eventloop();

    // Clean up devices in reversed order, something may reference older devices
    vector_foreach_back(machine->mmio_devs, i) {
        rvvm_mmio_free(vector_at(machine->mmio_devs, i));
    }

    vector_foreach_back(machine->harts, i) {
        riscv_hart_free(vector_at(machine->harts, i));
    }

    vector_free(machine->harts);
    vector_free(machine->mmio_devs);
    vector_free(machine->msi_targets);

    riscv_free_ram(&machine->mem);
    rvclose(machine->bootrom_file);
    rvclose(machine->kernel_file);
    rvclose(machine->dtb_file);
#ifdef USE_FDT
    fdt_node_free(machine->fdt);
#endif
    free(machine);
}

PUBLIC void rvvm_run_eventloop(void)
{
    rvvm_set_manual_eventloop(true);
    rvvm_eventloop((void*)(uint64_t)1);
    rvvm_set_manual_eventloop(false);
}

/*
 * RVVM Device API
 */

PUBLIC bool rvvm_mmio_none(rvvm_mmio_dev_t* dev, void* dest, uint64_t offset, uint8_t size)
{
    UNUSED(dev);
    UNUSED(offset);
    memset(dest, 0, size);
    return true;
}

PUBLIC bool rvvm_write_ram(rvvm_machine_t* machine, rvvm_addr_t dest, const void* src, uint64_t size)
{
    if (dest < machine->mem.addr
    || (dest - machine->mem.addr + size) > machine->mem.size) return false;
    memcpy(((uint8_t*)machine->mem.data) + (dest - machine->mem.addr), src, size);
    riscv_jit_mark_dirty_mem(machine, dest, size);
    return true;
}

PUBLIC bool rvvm_read_ram(rvvm_machine_t* machine, void* dest, rvvm_addr_t src, uint64_t size)
{
    if (src < machine->mem.addr
    || (src - machine->mem.addr + size) > machine->mem.size) return false;
    memcpy(dest, ((uint8_t*)machine->mem.data) + (src - machine->mem.addr), size);
    return true;
}

PUBLIC void* rvvm_get_dma_ptr(rvvm_machine_t* machine, rvvm_addr_t addr, uint64_t size)
{
    if (addr < machine->mem.addr
    || (addr - machine->mem.addr + size) > machine->mem.size) return NULL;
    riscv_jit_mark_dirty_mem(machine, addr, size);
    return ((uint8_t*)machine->mem.data) + (addr - machine->mem.addr);
}

static inline bool rvvm_mmio_overlap_check(rvvm_addr_t addr1, uint64_t size1, rvvm_addr_t addr2, uint64_t size2)
{
    return addr1 < (addr2 + size2) && addr2 < (addr1 + size1);
}

PUBLIC rvvm_addr_t rvvm_mmio_zone_auto(rvvm_machine_t* machine, rvvm_addr_t addr, uint64_t size)
{
    // Regions of size 0 are ignored (those are non-IO placeholders)
    if (size) {
        bool free_zone = false;
        while (!free_zone) {
            free_zone = true;
            if (rvvm_mmio_overlap_check(addr, size, machine->mem.addr, machine->mem.size)) {
                addr = (machine->mem.addr + machine->mem.size + 0xFFF) & ~0xFFFULL;
                free_zone = false;
                continue;
            }

            vector_foreach(machine->mmio_devs, i) {
                rvvm_mmio_dev_t* dev = vector_at(machine->mmio_devs, i);
                if (rvvm_mmio_overlap_check(addr, size, dev->addr, dev->size)) {
                    addr = (dev->addr + dev->size + 0xFFF) & ~0xFFFULL;
                    free_zone = false;
                    continue;
                }
            }
        }
    }
    return addr;
}

PUBLIC rvvm_mmio_dev_t* rvvm_attach_mmio(rvvm_machine_t* machine, const rvvm_mmio_dev_t* mmio_desc)
{
    rvvm_mmio_dev_t* dev = safe_new_obj(rvvm_mmio_dev_t);
    memcpy(dev, mmio_desc, sizeof(rvvm_mmio_dev_t));
    dev->machine = machine;

    // Normalize access properties: Power of two, default 1 - 8 bytes
    dev->min_op_size = dev->min_op_size ? bit_next_pow2(dev->min_op_size) : 1;
    dev->max_op_size = dev->max_op_size ? bit_next_pow2(dev->max_op_size) : 8;

    if (dev->min_op_size > dev->max_op_size || dev->min_op_size > 8) {
        rvvm_warn("MMIO device \"%s\" has invalid op sizes: min %u, max %u",
                  dev->type ? dev->type->name : "null", dev->min_op_size, dev->max_op_size);
        rvvm_mmio_free(dev);
        return NULL;
    }
    if (rvvm_mmio_zone_auto(machine, dev->addr, dev->size) != dev->addr) {
        rvvm_warn("Cannot attach MMIO device \"%s\" to occupied region 0x%08"PRIx64"",
                  dev->type ? dev->type->name : "null", dev->addr);
        rvvm_mmio_free(dev);
        return NULL;
    }
    if (dev->mapping && ((dev->addr & 0xFFF) ^ (((uint64_t)dev->mapping) & 0xFFF))) {
        // Misaligned mappings harm performance when used with KVM or shadow pagetable accel
        rvvm_warn("MMIO device \"%s\" has misaligned mapping, expect lower perf",
                  dev->type ? dev->type->name : "null");
    }

    rvvm_info("Attached MMIO device at 0x%08"PRIx64", type \"%s\"",
              dev->addr, dev->type ? dev->type->name : "null");

    bool was_running = rvvm_pause_machine(machine);
    vector_push_back(machine->mmio_devs, dev);
    if (was_running) rvvm_start_machine(machine);
    return dev;
}

PUBLIC void rvvm_remove_mmio(rvvm_mmio_dev_t* mmio_dev)
{
    if (mmio_dev == NULL) return;

    rvvm_machine_t* machine = mmio_dev->machine;
    bool was_running = rvvm_pause_machine(machine);

    // Remove from machine device list
    vector_foreach_back(machine->mmio_devs, i) {
        if (vector_at(machine->mmio_devs, i) == mmio_dev) {
            vector_erase(machine->mmio_devs, i);
        }
    }

    // It's a shared memory mapping, flush each hart TLB
    if (mmio_dev->mapping) {
        vector_foreach(machine->harts, i) {
            rvvm_hart_t* vm = vector_at(machine->harts, i);
            riscv_tlb_flush(vm);
        }
    }

    if (was_running) rvvm_start_machine(machine);

    rvvm_mmio_free(mmio_dev);
}

PUBLIC void rvvm_cleanup_mmio_desc(const rvvm_mmio_dev_t* mmio_desc)
{
    // Either device implements it's own cleanup routine, or we free it's data buffer
    rvvm_mmio_dev_t tmp_dev = *mmio_desc;
    if (tmp_dev.type && tmp_dev.type->remove) {
        tmp_dev.type->remove(&tmp_dev);
    } else {
        free(tmp_dev.data);
    }
}

PUBLIC rvvm_intc_t* rvvm_get_intc(rvvm_machine_t* machine)
{
    return machine->intc;
}

PUBLIC void rvvm_set_intc(rvvm_machine_t* machine, rvvm_intc_t* intc)
{
    if (intc) machine->intc = intc;
}

PUBLIC pci_bus_t* rvvm_get_pci_bus(rvvm_machine_t* machine)
{
    return machine->pci_bus;
}

PUBLIC void rvvm_set_pci_bus(rvvm_machine_t* machine, pci_bus_t* pci_bus)
{
    if (pci_bus) machine->pci_bus = pci_bus;
}

PUBLIC i2c_bus_t* rvvm_get_i2c_bus(rvvm_machine_t* machine)
{
    return machine->i2c_bus;
}

PUBLIC void rvvm_set_i2c_bus(rvvm_machine_t* machine, i2c_bus_t* i2c_bus)
{
    if (i2c_bus) machine->i2c_bus = i2c_bus;
}

PUBLIC struct fdt_node* rvvm_get_fdt_root(rvvm_machine_t* machine)
{
#ifdef USE_FDT
    return machine->fdt;
#else
    UNUSED(machine);
    return NULL;
#endif
}

PUBLIC struct fdt_node* rvvm_get_fdt_soc(rvvm_machine_t* machine)
{
#ifdef USE_FDT
    return machine->fdt_soc;
#else
    UNUSED(machine);
    return NULL;
#endif
}

/*
 * RVVM Interrupt API
 */

PUBLIC bool rvvm_send_msi_irq(rvvm_machine_t* machine, rvvm_addr_t addr, uint32_t val)
{
    if (machine) {
        uint32_t le = 0;
        write_uint32_le(&le, val);
        // Address must be aligned
        if (likely(!(addr & 3))) {
            vector_foreach(machine->msi_targets, i) {
                rvvm_mmio_dev_t* mmio = vector_at(machine->msi_targets, i);
                if (mmio->addr <= addr && addr < (mmio->addr + mmio->size)) {
                    return mmio->write(mmio, &le, addr - mmio->addr, sizeof(le));
                }
            }
        }
        rvvm_debug("Failed to send MSI IRQ %x to %"PRIx64, val, addr);
    }
    return false;
}

PUBLIC bool rvvm_attach_msi_target(rvvm_machine_t* machine, const rvvm_mmio_dev_t* mmio_desc)
{
    rvvm_mmio_dev_t* mmio = rvvm_attach_mmio(machine, mmio_desc);
    if (mmio) {
        if (mmio->min_op_size <= 4 && mmio->max_op_size >= 4) {
            vector_push_back(machine->msi_targets, mmio);
            return true;
        }
        rvvm_warn("MSI target %s has invalid op sizes!", mmio->type ? mmio->type->name : NULL);
    }
    return false;
}

PUBLIC rvvm_irq_t rvvm_alloc_irq(rvvm_intc_t* intc)
{
    if (intc) {
        if (intc->alloc_irq) {
            return intc->alloc_irq(intc);
        }
        return atomic_add_uint32(&intc->last_irq, 1) + 1;
    }
    return 0;
}

PUBLIC bool rvvm_send_irq(rvvm_intc_t* intc, rvvm_irq_t irq)
{
    if (likely(intc)) {
        return intc->send_irq(intc, irq);
    }
    return false;
}

PUBLIC bool rvvm_raise_irq(rvvm_intc_t* intc, rvvm_irq_t irq)
{
    if (likely(intc)) {
        if (likely(intc->raise_irq)) {
            return intc->raise_irq(intc, irq);
        }
    }
    return rvvm_send_irq(intc, irq);
}

PUBLIC bool rvvm_lower_irq(rvvm_intc_t* intc, rvvm_irq_t irq)
{

    if (likely(intc && intc->lower_irq)) {
        return intc->lower_irq(intc, irq);
    }
    return false;
}

PUBLIC bool rvvm_fdt_describe_irq(struct fdt_node* node, rvvm_intc_t* intc, rvvm_irq_t irq)
{
#ifdef USE_FDT
    if (intc) {
        uint32_t cells[8] = {0};
        uint64_t count = rvvm_fdt_irq_cells(intc, irq, cells, STATIC_ARRAY_SIZE(cells));
        fdt_node_add_prop_u32(node, "interrupt-parent", rvvm_fdt_intc_phandle(intc));
        fdt_node_add_prop_cells(node, "interrupts", cells, count);
        return true;
    }
#else
    UNUSED(node); UNUSED(intc); UNUSED(irq);
#endif
    return false;
}

//! \brief Get interrupt-parent FDT phandle of an interrupt controller
PUBLIC uint32_t rvvm_fdt_intc_phandle(rvvm_intc_t* intc)
{
    if (intc && intc->fdt_phandle) {
        return intc->fdt_phandle(intc);
    }
    return 0;
}

//! \brief Get interrupts-extended FDT cells for an IRQ
PUBLIC uint64_t rvvm_fdt_irq_cells(rvvm_intc_t* intc, rvvm_irq_t irq, uint32_t* cells, uint64_t size)
{
    if (intc && intc->fdt_irq_cells) {
        return intc->fdt_irq_cells(intc, irq, cells, size);
    }
    return 0;
}

/*
 * Userland emulation API (WIP)
 */

PUBLIC rvvm_machine_t* rvvm_create_userland(const char* isa)
{
    // TODO: Proper full ISA string parsing
    if (!isa) isa = "rv64";
    bool rv64 = rvvm_strfind(isa, "rv64") == isa;
    bool rv32 = rvvm_strfind(isa, "rv32") == isa;
    if (!rv64 && !rv32) {
        rvvm_error("Invalid CPU ISA string: %s", isa);
        return NULL;
    }

    rvvm_machine_t* machine = safe_new_obj(rvvm_machine_t);

    // Bypass entire process memory except the NULL page
    // RVVM expects mem.data to be non-NULL, let's leave that for now
    machine->mem.addr = 0x1000;
    machine->mem.size = (uint64_t)-0x1000ULL;
    machine->mem.data = (void*)0x1000;
    machine->rv64 = rv64;

    // Set nanosecond machine timer precision by default
    rvvm_set_opt(machine, RVVM_OPT_TIME_FREQ, 1000000000ULL);

#ifdef USE_JIT
    rvvm_set_opt(machine, RVVM_OPT_JIT, true);
    rvvm_set_opt(machine, RVVM_OPT_JIT_HARVARD, true);
    rvvm_set_opt(machine, RVVM_OPT_JIT_CACHE, 16 << 20);
#endif
    return machine;
}

PUBLIC void rvvm_flush_icache(rvvm_machine_t* machine, rvvm_addr_t addr, uint64_t size)
{
    // WIP, issue a total cache flush on all harts
    // Needs improvements in RVJIT
    UNUSED(addr);
    UNUSED(size);
    spin_lock(&global_lock);
    vector_foreach(machine->harts, i) {
        riscv_jit_flush_cache(vector_at(machine->harts, i));
    }
    spin_unlock(&global_lock);
}

PUBLIC rvvm_hart_t* rvvm_create_user_thread(rvvm_machine_t* machine)
{
    rvvm_hart_t* thread = riscv_hart_init(machine);
    riscv_hart_prepare(thread);
    // Allow time CSR access from U-mode
    thread->csr.counteren[RISCV_PRIV_MACHINE] = CSR_COUNTEREN_MASK;
    thread->csr.counteren[RISCV_PRIV_SUPERVISOR] = CSR_COUNTEREN_MASK;

    // Allow Zkr seed CSR access from U-mode
    thread->csr.mseccfg = CSR_MSECCFG_MASK;

    // Allow Zicboz/Zicbom access from U-mode
    thread->csr.envcfg[RISCV_PRIV_MACHINE] = CSR_MENVCFG_MASK;
    thread->csr.envcfg[RISCV_PRIV_SUPERVISOR] = CSR_SENVCFG_MASK;
#ifdef USE_FPU
    // Initialize FPU by writing to status CSR
    rvvm_uxlen_t mstatus = (FS_INITIAL << 13);
    riscv_csr_op(thread, CSR_MSTATUS, &mstatus, CSR_SETBITS);
#endif
#ifdef USE_JIT
    // Enable pointer optimization
    rvjit_set_native_ptrs(&thread->jit, true);
#endif
    riscv_switch_priv(thread, RISCV_PRIV_USER);
    spin_lock(&global_lock);
    if (!vector_size(machine->harts)) {
        // This is the main thread, initialize the timer
        rvtimer_init(&machine->timer, rvvm_get_opt(machine, RVVM_OPT_TIME_FREQ));
    }
    vector_push_back(machine->harts, thread);
    spin_unlock(&global_lock);
    return thread;
}

PUBLIC void rvvm_free_user_thread(rvvm_hart_t* thread)
{
    spin_lock(&global_lock);
    vector_foreach(thread->machine->harts, i) {
        if (vector_at(thread->machine->harts, i) == thread) {
            vector_erase(thread->machine->harts, i);
            riscv_hart_free(thread);
            spin_unlock(&global_lock);
            return;
        }
    }
    rvvm_fatal("Corrupted userland context!");
}

PUBLIC rvvm_addr_t rvvm_run_user_thread(rvvm_hart_t* thread)
{
    return riscv_hart_run_userland(thread);
}

PUBLIC rvvm_addr_t rvvm_read_cpu_reg(rvvm_hart_t* thread, uint64_t reg_id)
{
    if (reg_id < (RVVM_REGID_X0 + 32)) {
        return thread->registers[reg_id - RVVM_REGID_X0];
#ifdef USE_FPU
    } else if (reg_id < (RVVM_REGID_F0 + 32)) {
        rvvm_addr_t ret;
        memcpy(&ret, &thread->fpu_registers[reg_id - RVVM_REGID_F0], sizeof(ret));
        return ret;
#endif
    } else if (reg_id == RVVM_REGID_PC) {
        return thread->registers[RISCV_REG_PC];
    } else if (reg_id == RVVM_REGID_CAUSE) {
        return thread->csr.cause[RISCV_PRIV_USER];
    } else if (reg_id == RVVM_REGID_TVAL) {
        return thread->csr.tval[RISCV_PRIV_USER];
    } else {
        rvvm_warn("Unknown register %d in rvvm_read_cpu_reg()!", (uint32_t)reg_id);
        return 0;
    }
}

PUBLIC void rvvm_write_cpu_reg(rvvm_hart_t* thread, uint64_t reg_id, rvvm_addr_t reg)
{
    if (reg_id < (RVVM_REGID_X0 + 32)) {
        thread->registers[reg_id - RVVM_REGID_X0] = reg;
#ifdef USE_FPU
    } else if (reg_id < (RVVM_REGID_F0 + 32)) {
        memcpy(&thread->fpu_registers[reg_id - RVVM_REGID_F0], &reg, sizeof(reg));
#endif
    } else if (reg_id == RVVM_REGID_PC) {
        thread->registers[RISCV_REG_PC] = reg;
    } else if (reg_id == RVVM_REGID_CAUSE) {
        thread->csr.cause[RISCV_PRIV_USER] = reg;
    } else if (reg_id == RVVM_REGID_TVAL) {
        thread->csr.tval[RISCV_PRIV_USER] = reg;
    } else {
        rvvm_warn("Unknown register %d in rvvm_write_cpu_reg()!", (uint32_t)reg_id);
    }
}

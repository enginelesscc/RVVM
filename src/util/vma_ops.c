/*
vma_ops.c - Virtual memory area operations
Copyright (C) 2023  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Expose syscall(), sysconf(), madvise(), mremap(), ftruncate(), MAP_ANON
#include "feature_test.h"

#include "blk_io.h"
#include "compiler.h"
#include "mem_ops.h"
#include "rvvm_types.h"
#include "spinlock.h"
#include "utils.h"
#include "vma_ops.h"

PUSH_OPTIMIZATION_SIZE

#if !defined(USE_VMA_DUMMY) && defined(HOST_TARGET_WIN32)

// Win32 VMA implementation using VirtualAlloc(), VirtualFree(), VirtualProtect(), MapViewOfFile(), etc
#include <windows.h>

/*
 * TODO: Better mmap() emulation on Win32?
 * - Proper vma_remap()
 * - Partial unmapping
 * - Handle non-granular fixed-address file mappings
 */

#ifndef FILE_MAP_EXECUTE
#define FILE_MAP_EXECUTE 0x20
#endif
#ifndef MEM_LARGE_PAGES
#define MEM_LARGE_PAGES 0x20000000UL
#endif

static inline DWORD vma_native_prot(uint32_t flags)
{
    switch (flags & VMA_RWX) {
        case VMA_EXEC:
            return PAGE_EXECUTE;
        case VMA_READ:
            return PAGE_READONLY;
        case VMA_RDEX:
            return PAGE_EXECUTE_READ;
        case VMA_WRITE:
        case VMA_RDWR:
            return PAGE_READWRITE;
        case VMA_EXEC | VMA_WRITE:
        case VMA_RWX:
            return PAGE_EXECUTE_READWRITE;
    }
    return PAGE_NOACCESS;
}

static inline DWORD vma_native_view_prot(uint32_t flags)
{
    DWORD ret = 0;
    if (!(flags & VMA_SHARED) && (flags & VMA_WRITE)) {
        // Set FILE_MAP_COPY on private writable mappings
        return FILE_MAP_COPY;
    }
    ret |= (flags & VMA_EXEC) ? FILE_MAP_EXECUTE : 0;
    ret |= (flags & VMA_READ) ? FILE_MAP_READ : 0;
    ret |= (flags & VMA_WRITE) ? FILE_MAP_WRITE : 0;
    return ret;
}

#define VMA_WIN32_IMPL 1

#if defined(HOST_TARGET_WINNT) && defined(EXCEPTION_ACCESS_VIOLATION)

/*
 * Pause SEH access violation handling in all threads at own will
 * Prevents a crash when a thread accesses a VMA that's currently being zeroed by vma_clean()
 * Evil ideas require evil solutions...
 */

#define VMA_WIN32_SEH_IMPL 1

#include "spinlock.h"

static LPTOP_LEVEL_EXCEPTION_FILTER seh_prev_handler = NULL;

static spinlock_t seh_lock = ZERO_INIT;

static void seh_revert_handler(void)
{
    if (seh_prev_handler) {
        SetUnhandledExceptionFilter(seh_prev_handler);
        seh_prev_handler = NULL;
    }
}

static LONG CALLBACK seh_handler(EXCEPTION_POINTERS* ptrs)
{
    LONG ret = 0;
    scoped_spin_lock_slow (&seh_lock) {
        if (ptrs->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            seh_revert_handler();
            ret = -1;
        } else if (seh_prev_handler) {
            ret = seh_prev_handler(ptrs);
        }
    }
    return ret;
}

// NOTE: Must be called under seh_lock!
static void seh_suspend_access_violation(void)
{
    seh_prev_handler = SetUnhandledExceptionFilter(seh_handler);
    DO_ONCE(call_at_deinit(seh_revert_handler));
}

#endif

#elif !defined(USE_VMA_DUMMY) && defined(HOST_TARGET_POSIX) && HOST_TARGET_POSIX >= 199309L

// POSIX 1003.1b-1993 VMA implementation using mmap(), munmap(), madvise(), etc
#include <fcntl.h>    // For open(), O_*
#include <sys/mman.h> // For mmap(), munmap(), mprotect(), shm_open(), shm_unlink(), PROT_*, MAP_*, MFD_CLOEXEC
#include <unistd.h>   // For sysconf(_SC_PAGESIZE), syscall(), close(), unlink(), ftruncate()

#if defined(HOST_TARGET_LINUX)
#include <sys/syscall.h> // For __NR_memfd_create, __NR_membarrier
#endif

#if defined(HOST_TARGET_SERENITY)
#include <serenity.h> // For anon_create()
#endif

#if defined(HOST_TARGET_DARWIN) && defined(__aarch64__)
#include <mach/task.h>         // For mach_msg_type_number_t, thread_act_t, task_threads()
#include <mach/thread_state.h> // For mach_task_self(), thread_get_register_pointer_values()
#include <mach/vm_map.h>       // For vm_address_t, vm_deallocate()
#else
// Passing MAP_JIT on non-ARM64 is pointless, and causes an mmap() error on old Darwin
#undef MAP_JIT
#endif

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_JIT
#define MAP_JIT 0
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef MAP_FIXED
#define MAP_FIXED 0
#endif
#ifndef MAP_NOSYNC
#define MAP_NOSYNC 0
#endif

#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif

#ifndef O_EXCL
#define O_EXCL 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#ifndef PROT_NONE
#define PROT_NONE 0
#endif

static inline int vma_native_prot(uint32_t flags)
{
    int ret  = 0;
    ret     |= (flags & VMA_EXEC) ? PROT_EXEC : 0;
    ret     |= (flags & VMA_READ) ? PROT_READ : 0;
    ret     |= (flags & VMA_WRITE) ? PROT_WRITE : 0;
    return ret ? ret : PROT_NONE;
}

#define VMA_MMAP_IMPL 1

#else

#pragma message("Falling back to dummy VMA implementation")

#endif

static size_t host_pagesize    = 0;
static size_t host_granularity = 0; // Allocation granularity, may be >= pagesize

static void vma_page_size_init_once(void)
{
#if defined(VMA_WIN32_IMPL)
    SYSTEM_INFO info = {
        .dwPageSize              = 0x1000,
        .dwAllocationGranularity = 0x10000,
    };
    GetSystemInfo(&info);
    host_pagesize    = info.dwPageSize;
    host_granularity = info.dwAllocationGranularity;
#elif defined(VMA_MMAP_IMPL)
    host_pagesize = sysconf(_SC_PAGESIZE);
#if defined(HOST_TARGET_COSMOPOLITAN) && defined(_SC_GRANSIZE)
    // Support Windows allocation granularity on Cosmopolitan
    host_granularity = sysconf(_SC_GRANSIZE);
#endif
#else
    // Non-paging fallback via malloc/free, disable alignment
    host_pagesize    = 1;
    host_granularity = 1;
#endif
    if (!host_pagesize || host_pagesize > 0x100000) {
        host_pagesize = 0x1000;
    }
    if (!host_granularity || host_granularity > 0x100000) {
        host_granularity = host_pagesize;
    }
}

static void vma_page_size_init(void)
{
    DO_ONCE(vma_page_size_init_once());
}

size_t vma_page_size(void)
{
    vma_page_size_init();
    return host_pagesize;
}

static size_t vma_granularity(void)
{
    vma_page_size_init();
    return host_granularity;
}

static inline void vma_align_outward(void** addr, size_t* size)
{
#if defined(VMA_MMAP_IMPL) || defined(VMA_WIN32_IMPL)
    size_t ptr_diff = ((size_t)*addr) & (vma_granularity() - 1);
    *addr           = ((char*)*addr) - ptr_diff;
    *size           = align_size_up(*size + ptr_diff, vma_page_size());
#endif
    UNUSED(addr);
    UNUSED(size);
}

static inline void vma_align_inward(void** addr, size_t* size)
{
#if defined(VMA_MMAP_IMPL) || defined(VMA_WIN32_IMPL)
    size_t ptr_diff = (vma_page_size() - ((size_t)*addr)) & (vma_page_size() - 1);
    *addr           = ((char*)*addr) + ptr_diff;
    *size           = align_size_down(*size - ptr_diff, vma_page_size());
#endif
    UNUSED(addr);
    UNUSED(size);
}

int vma_anon_memfd(size_t size)
{
    int memfd = -1;
#if defined(VMA_MMAP_IMPL)
    size = align_size_up(size, vma_granularity());
#if defined(HOST_TARGET_LINUX) && defined(__NR_memfd_create) && defined(MFD_CLOEXEC)
    memfd = syscall(__NR_memfd_create, "vma_anon", MFD_CLOEXEC);
#elif defined(HOST_TARGET_FREEBSD) && defined(SHM_ANON)
    memfd = shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0);
#elif defined(HOST_TARGET_SERENITY)
    memfd = anon_create(size, O_CLOEXEC);
    if (memfd >= 0) {
        return memfd;
    }
#endif

    // Try exclusively opening random names via shm_open() where supported, and in various tmpfs-like places
    // NOTE: This will fail under isolation!
    for (size_t i = 0; memfd < 0 && i < 10; ++i) {
        char path[256]         = {0};
        char random_suffix[32] = {0};
        rvvm_randomserial(random_suffix, 16);
#if HOST_TARGET_POSIX >= 199506L && !defined(HOST_TARGET_ANDROID) && !defined(HOST_TARGET_SERENITY)
        if (i < 5) {
            size_t off = rvvm_strlcpy(path, "/shm-vma-anon-", sizeof(path));
            rvvm_strlcpy(path + off, random_suffix, sizeof(path) - off);
            memfd = shm_open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (memfd >= 0 && shm_unlink(path)) {
                close(memfd);
                memfd = -1;
            }
        }
#endif
        if (memfd < 0) {
            const char* tmp_dirs[] = {NULL, "/var/tmp", "/tmp"};
            tmp_dirs[0]            = getenv("XDG_RUNTIME_DIR");
            for (size_t j = 0; memfd < 0 && j < STATIC_ARRAY_SIZE(tmp_dirs); ++j) {
                const char* dir = tmp_dirs[j];
                if (memfd < 0 && dir) {
                    size_t off  = rvvm_strlcpy(path, dir, sizeof(path));
                    off        += rvvm_strlcpy(path + off, "/vma-anon-", sizeof(path) - off);
                    rvvm_strlcpy(path + off, random_suffix, sizeof(path) - off);
                    memfd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
                }
            }
            if (memfd >= 0 && unlink(path)) {
                close(memfd);
                memfd = -1;
            }
        }
    }

    // Resize anon FD
    if (memfd >= 0 && ftruncate(memfd, size) < 0) {
        close(memfd);
        memfd = -1;
    }

#else
    UNUSED(size);
    rvvm_warn("Anonymous memfd is not supported!");
#endif
    return memfd;
}

bool vma_broadcast_membarrier(void)
{
#if defined(VMA_WIN32_IMPL) && (defined(__aarch64__) || defined(_M_ARM64))
    // Use FlushProcessWriteBuffers() on Windows ARM64
    FlushProcessWriteBuffers();
    return true;
#else
#if defined(VMA_MMAP_IMPL) && defined(HOST_TARGET_LINUX) && defined(__NR_membarrier)
    static uint32_t has_membarrier = 0;
    // Register intent to use private expedited membarrier
    DO_ONCE_SCOPED {
        atomic_store_uint32_relax(&has_membarrier, !syscall(__NR_membarrier, 0x10, 0));
    };
    if (atomic_load_uint32_relax(&has_membarrier)) {
        // Perform private expedited membarrier
        if (!syscall(__NR_membarrier, 0x8, 0)) {
            return true;
        }
        atomic_store_uint32_relax(&has_membarrier, false);
    }
#elif defined(VMA_MMAP_IMPL) && defined(HOST_TARGET_DARWIN) && defined(__aarch64__)
    // Request stack pointer value on each thread to enforce a global memory barrier
    mach_msg_type_number_t num_threads = 0;
    thread_act_t*          ptr_threads = NULL;
    bool                   ret         = true;
    if (!task_threads(mach_task_self(), &ptr_threads, &num_threads)) {
        uintptr_t sp        = 0;
        uintptr_t regs[128] = {0};
        for (size_t i = 0; i < num_threads; ++i) {
            size_t num_regs = 128;
            if (thread_get_register_pointer_values(ptr_threads[i], &sp, &num_regs, regs)) {
                ret = false;
            }
        }
        vm_deallocate(mach_task_self(), (vm_address_t)ptr_threads, num_threads * sizeof(thread_act_t));
        return ret;
    }
#endif
#if (defined(VMA_MMAP_IMPL) || defined(VMA_WIN32_IMPL)) && !defined(__aarch64__) && !defined(_M_ARM64)
    // Most OS kernels perform an IPI for mprotect()/munmap(), though on ARM64 this is not guaranteed due to tlbi
    void* ipi_page = vma_alloc(NULL, vma_page_size(), VMA_RDWR);
    if (ipi_page) {
        memset(ipi_page, 0, 4);
        bool ret = vma_protect(ipi_page, vma_page_size(), VMA_READ);
        vma_free(ipi_page, vma_page_size());
        return ret;
    }
#endif
    return false;
#endif
}

static void* vma_mmap_aligned_internal(void* addr, size_t size, uint32_t flags, rvfile_t* file, uint64_t offset)
{
    void* ret = NULL;
#if defined(VMA_WIN32_IMPL)
    // Win32 implementation
    if (file) {
        HANDLE file_handle = rvfile_get_win32_handle(file);
        if ((flags & VMA_SHARED) && (flags & VMA_WRITE) && (flags & VMA_EXEC)) {
            // Shared writable executable mappings are a no-go
            return NULL;
        }
        if (file_handle) {
#if defined(HOST_TARGET_WIN9X)
            // Use ANSI syscall (Win9x compat)
            HANDLE file_map = CreateFileMappingA(file_handle, NULL, vma_native_prot(flags), 0, 0, NULL);
#else
            HANDLE file_map = CreateFileMappingW(file_handle, NULL, vma_native_prot(flags), 0, 0, NULL);
#endif
            if (!file_map || file_map == INVALID_HANDLE_VALUE) {
                return NULL;
            }
#if defined(HOST_TARGET_WINNT)
            // No MapViewOfFileEx() on Windows CE
            if (flags & VMA_FIXED) {
                ret = MapViewOfFileEx(file_map, vma_native_view_prot(flags), //
                                      offset >> 32, (uint32_t)offset, size, addr);
            }
#endif
            if (!ret) {
                ret = MapViewOfFile(file_map, vma_native_view_prot(flags), offset >> 32, (uint32_t)offset, size);
            }
            CloseHandle(file_map);
        }
    } else {
        ret = VirtualAlloc(addr, size, MEM_COMMIT | MEM_RESERVE, vma_native_prot(flags));
    }

#elif defined(VMA_MMAP_IMPL)
    // POSIX mmap() implementation
    int mmap_flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | ((flags & VMA_EXEC) ? MAP_JIT : 0);
    int mmap_fd    = file ? rvfile_get_posix_fd(file) : -1;
    if (file) {
        mmap_flags = MAP_NORESERVE | ((flags & VMA_SHARED) ? MAP_SHARED : MAP_PRIVATE);
        if (mmap_fd == -1) {
            // File doesn't have a native POSIX fd
            return NULL;
        }
    }
    if (flags & VMA_FIXED) {
        // Use non-destructive MAP_FIXED semantics, even on Linux
#if defined(MAP_FIXED_NOREPLACE)
        mmap_flags |= MAP_FIXED_NOREPLACE;
#elif defined(MAP_TRYFIXED)
        mmap_flags |= MAP_TRYFIXED;
#elif defined(MAP_EXCL)
        mmap_flags |= MAP_FIXED | MAP_EXCL;
#else
        mmap_flags |= MAP_FIXED;
#endif
    }
    ret = mmap(addr, size, vma_native_prot(flags), mmap_flags, mmap_fd, offset);
    if (ret == MAP_FAILED) {
        ret = NULL;
    }
    // Apply madvise() flags
#if defined(HOST_TARGET_LINUX) && defined(MADV_MERGEABLE)
    if (ret && (flags & VMA_KSM)) {
        madvise(ret, size, MADV_MERGEABLE);
    }
#endif
#if defined(HOST_TARGET_LINUX) && defined(MADV_HUGEPAGE)
    if (ret && (flags & VMA_THP)) {
        madvise(ret, size, MADV_HUGEPAGE);
    }
#endif

#else
    // Generic libc implementation using calloc()
    // No support for VMA_SHARED, VMA_EXEC, VMA_FIXED
    UNUSED(addr);
    UNUSED(offset);
    if (!(flags & (VMA_SHARED | VMA_EXEC | VMA_FIXED)) && !file) {
        ret = calloc(size, 1);
    }
#endif
    return ret;
}

static void* vma_mmap_aligned(void* addr, size_t size, uint32_t flags, rvfile_t* file, uint64_t offset)
{
    void* ret = vma_mmap_aligned_internal(addr, size, flags, file, offset);
    if (!ret && file) {
        // Private file mappings emulation fallback
        ret = vma_mmap_aligned_internal(addr, size, VMA_RDWR | (flags & VMA_FIXED), NULL, 0);
        if (ret && (rvread(file, ret, size, offset) != size || !vma_protect(addr, size, flags & VMA_RWX))) {
            vma_free(ret, size);
            return NULL;
        }
    }
    return ret;
}

void* vma_alloc(void* addr, size_t size, uint32_t flags)
{
    return vma_mmap(addr, size, flags, NULL, 0);
}

void* vma_alloc_large_ram(size_t size, size_t* page_size)
{
    *page_size = 0;
#if defined(VMA_WIN32_IMPL) && defined(HOST_TARGET_WINNT)
    // Resolve optional APIs to preserve startup compatibility with older Windows.
    typedef SIZE_T (WINAPI *large_min_t)(void);
    typedef BOOL (WINAPI *open_token_t)(HANDLE, DWORD, PHANDLE);
    typedef BOOL (WINAPI *lookup_priv_t)(LPCWSTR, LPCWSTR, PLUID);
    typedef BOOL (WINAPI *adjust_priv_t)(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD);
    large_min_t large_min = (large_min_t)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetLargePageMinimum");
    size_t minimum = large_min ? large_min() : 0;
    if (!minimum || !size || size % minimum) {
        rvvm_warn("Large-page RAM unavailable: size must be a nonzero multiple of %" PRIu64 " bytes", (uint64_t)minimum);
        return NULL;
    }
    // Serialize our temporary changes to the process token across machine creations.
    static spinlock_t large_ram_lock = ZERO_INIT;
    spin_lock_slow(&large_ram_lock);
    HMODULE advapi = LoadLibraryW(L"advapi32.dll");
    open_token_t open_token = advapi ? (open_token_t)(void*)GetProcAddress(advapi, "OpenProcessToken") : NULL;
    lookup_priv_t lookup_priv = advapi ? (lookup_priv_t)(void*)GetProcAddress(advapi, "LookupPrivilegeValueW") : NULL;
    adjust_priv_t adjust_priv = advapi ? (adjust_priv_t)(void*)GetProcAddress(advapi, "AdjustTokenPrivileges") : NULL;
    HANDLE token = NULL;
    TOKEN_PRIVILEGES desired = {0}, previous = {0};
    DWORD length = sizeof(previous), error = ERROR_CALL_NOT_IMPLEMENTED;
    void* ret = NULL;
    if (open_token && lookup_priv && adjust_priv) {
        desired.PrivilegeCount = 1;
        desired.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (open_token(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)
            && lookup_priv(NULL, L"SeLockMemoryPrivilege", &desired.Privileges[0].Luid)) {
            SetLastError(ERROR_SUCCESS);
            BOOL adjusted = adjust_priv(token, FALSE, &desired, sizeof(previous), &previous, &length);
            error = GetLastError(); // TRUE with ERROR_NOT_ALL_ASSIGNED is still failure.
            if (adjusted && error == ERROR_SUCCESS) {
                ret = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
                error = ret ? ERROR_SUCCESS : GetLastError();
                // Restore the process token; never grant an account right or change system policy.
                if (previous.PrivilegeCount && !adjust_priv(token, FALSE, &previous, 0, NULL, NULL)) {
                    rvvm_warn("Failed to restore large-page token privilege: %lu", (unsigned long)GetLastError());
                }
            }
        } else {
            error = GetLastError();
        }
    }
    if (token) CloseHandle(token);
    if (advapi) FreeLibrary(advapi);
    spin_unlock(&large_ram_lock);
    if (ret) {
        *page_size = minimum;
    } else {
        rvvm_warn("Large-page RAM unavailable: Win32 error %lu; falling back to ordinary pages", (unsigned long)error);
    }
    return ret;
#else
    UNUSED(size);
    rvvm_warn("Explicit large-page RAM is only implemented on Windows NT; using ordinary allocation");
    return NULL;
#endif
}

void* vma_mmap(void* addr, size_t size, uint32_t flags, rvfile_t* file, uint64_t offset)
{
    uint8_t* ret      = NULL;
    size_t   ptr_diff = ((size_t)addr) & (vma_granularity() - 1);

    if (file) {
        // File VMA mapping
        size_t off_diff = offset & (vma_granularity() - 1);
        if ((flags & VMA_FIXED) && (ptr_diff != off_diff)) {
            // Misaligned address / offset passed with VMA_FIXED
            return NULL;
        } else {
            // Fixup file offset misalign
            addr      = NULL;
            offset   -= off_diff;
            ptr_diff  = off_diff;
        }
    } else {
        // Anonymous VMA allocation
        offset = 0;
        if (flags & VMA_SHARED) {
            // Mapping shared anonymous memory doesn't make a lot of sense
            return NULL;
        }
    }

    if (addr) {
        addr = ((char*)addr) - ptr_diff;
    }

    size = align_size_up(size + ptr_diff, vma_page_size());

    if (file && offset + size > rvfilesize(file)) {
        // Grow the file to fit the mapping for same semantics across systems
        if (!rvfallocate(file, offset + size)) {
            return NULL;
        }
    }

    ret = vma_mmap_aligned(addr, size, flags, file, offset);

    if ((flags & VMA_FIXED) && ret && ret != addr) {
        vma_free(ret, size);
        ret = NULL;
    }

    return ret ? (ret + ptr_diff) : NULL;
}

bool vma_multi_mmap(void** rw, void** exec, size_t size)
{
    size = align_size_up(size, vma_granularity());
#if defined(VMA_MMAP_IMPL)
    int memfd = vma_anon_memfd(size);
    if (memfd > 0) {
        *rw   = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOSYNC, memfd, 0);
        *exec = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_NOSYNC, memfd, 0);
        close(memfd);
        if (*rw != MAP_FAILED && *exec != MAP_FAILED) {
            return true;
        }
        if (*rw != MAP_FAILED) {
            munmap(*rw, size);
        }
        if (*exec != MAP_FAILED) {
            munmap(*exec, size);
        }
    }
    *rw   = NULL;
    *exec = NULL;
    return false;
#else
    UNUSED(rw);
    UNUSED(exec);
    UNUSED(size);
    return false;
#endif
}

void* vma_remap(void* addr, size_t old_size, size_t new_size, uint32_t flags)
{
    uint8_t* ret = NULL;

    // Align VMA outward
    size_t ptr_diff = ((size_t)addr) & (vma_granularity() - 1);
    addr            = ((char*)addr) - ptr_diff;
    old_size        = align_size_up(old_size + ptr_diff, vma_page_size());
    new_size        = align_size_up(new_size + ptr_diff, vma_page_size());

    if (new_size == old_size) {
        return addr;
    }

#if defined(VMA_MMAP_IMPL) && defined(HOST_TARGET_LINUX) && defined(MREMAP_MAYMOVE)
    ret = mremap(addr, old_size, new_size, (flags & VMA_FIXED) ? 0 : MREMAP_MAYMOVE);
    if (ret == MAP_FAILED) {
        ret = NULL;
    }
#endif
#if defined(VMA_MMAP_IMPL)
    if (!ret && new_size < old_size) {
        // Shrink the mapping by unmapping at the end
        if (vma_free(((uint8_t*)addr) + new_size, old_size - new_size)) {
            ret = addr;
        }
    } else if (!ret && new_size > old_size) {
        // Grow the mapping by mapping additional pages at the end
        if (vma_alloc(((uint8_t*)addr) + old_size, new_size - old_size, flags | VMA_FIXED)) {
            ret = addr;
        }
    }
#endif
    if (!ret && !(flags & VMA_FIXED)) {
        // Just copy the data into a completely new mapping
        ret = vma_alloc(NULL, new_size, flags);
        if (ret) {
            memcpy(ret, addr, EVAL_MIN(old_size, new_size));
            vma_free(addr, old_size);
        }
    }

    return ret ? (ret + ptr_diff) : NULL;
}

bool vma_protect(void* addr, size_t size, uint32_t flags)
{
    vma_align_inward(&addr, &size);
    if (addr && size) {
#if defined(VMA_WIN32_IMPL)
        DWORD old = 0;
        return VirtualProtect(addr, size, vma_native_prot(flags), &old);
#elif defined(VMA_MMAP_IMPL)
        return !mprotect(addr, size, vma_native_prot(flags));
#else
        return flags == VMA_RDWR;
#endif
    }
    return false;
}

bool vma_sync(void* addr, size_t size)
{
    vma_align_outward(&addr, &size);
    if (addr && size) {
#if defined(VMA_WIN32_IMPL)
        return FlushViewOfFile(addr, size);
#elif defined(VMA_MMAP_IMPL) && defined(MS_SYNC)
        return !msync(addr, size, MS_SYNC);
#else
        return true;
#endif
    }
    return false;
}

bool vma_clean(void* addr, size_t size, bool lazy)
{
    vma_align_inward(&addr, &size);
    if (addr && size) {
#if defined(VMA_WIN32_IMPL) && defined(VMA_WIN32_SEH_IMPL)
        MEMORY_BASIC_INFORMATION mbi = {0};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.Type == MEM_PRIVATE) {
            scoped_spin_lock_slow (&seh_lock) {
                seh_suspend_access_violation();
                if (!VirtualFree(addr, size, MEM_DECOMMIT)) {
                    rvvm_fatal("vma_clean(): VirtualFree() failed!");
                }
                if (!VirtualAlloc(addr, size, MEM_COMMIT, mbi.Protect)) {
                    rvvm_fatal("vma_clean(): VirtualAlloc() failed!");
                }
            }
            return true;
        }
#elif defined(VMA_MMAP_IMPL) && defined(HOST_TARGET_LINUX) && defined(MADV_DONTNEED)
        if (!madvise(addr, size, MADV_DONTNEED)) {
            return true;
        }
#elif defined(VMA_MMAP_IMPL) && defined(HOST_TARGET_DARWIN) && defined(MADV_FREE_REUSABLE)
        madvise(addr, size, MADV_FREE_REUSABLE);
#elif defined(VMA_MMAP_IMPL) && defined(MADV_FREE)
        madvise(addr, size, MADV_FREE);
#endif
        return lazy;
    }
    return false;
}

bool vma_pageout(void* addr, size_t size, bool lazy)
{
    vma_align_inward(&addr, &size);
    if (addr && size) {
#if defined(VMA_WIN32_IMPL) && defined(HOST_TARGET_WINNT)
        if (VirtualUnlock(addr, size) || GetLastError() == ERROR_NOT_LOCKED) {
            return true;
        }
#elif defined(VMA_MMAP_IMPL) && defined(HOST_TARGET_LINUX) && defined(MADV_PAGEOUT) && defined(MADV_COLD)
        if (!lazy && !madvise(addr, size, MADV_PAGEOUT)) {
            return true;
        }
        madvise(addr, size, MADV_COLD);
#elif defined(VMA_MMAP_IMPL) && defined(POSIX_MADV_DONTNEED)
        posix_madvise(addr, size, POSIX_MADV_DONTNEED);
#endif
        return lazy;
    }
    return false;
}

bool vma_free(void* addr, size_t size)
{
    vma_align_outward(&addr, &size);
    if (addr && size) {
#if defined(VMA_WIN32_IMPL)
        MEMORY_BASIC_INFORMATION mbi = {0};
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) {
            rvvm_warn("vma_free(): VirtualQuery() failed!");
            return false;
        }
        if (mbi.AllocationBase != addr) {
            rvvm_warn("vma_free(): Invalid VMA address: %p != %p", mbi.AllocationBase, addr);
        }
        if (mbi.RegionSize != size) {
            rvvm_warn("vma_free(): Invalid VMA size: %llx != %llx", (long long)mbi.RegionSize, (long long)size);
        }
        if (mbi.Type == MEM_MAPPED) {
            return UnmapViewOfFile(addr);
        } else if (mbi.Type == MEM_PRIVATE) {
            return VirtualFree(addr, 0, MEM_RELEASE);
        } else {
            rvvm_warn("vma_free(): Invalid win32 page type %x!", (uint32_t)mbi.Type);
        }
#elif defined(VMA_MMAP_IMPL)
        return !munmap(addr, size);
#else
        free(addr);
        return true;
#endif
    }
    return false;
}

POP_OPTIMIZATION_SIZE

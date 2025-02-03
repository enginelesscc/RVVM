/*
utils.с - Util functions
Copyright (C) 2021  LekKit <github.com/LekKit>
                    0xCatPKG <0xCatPKG@rvvm.dev>
                    0xCatPKG <github.com/0xCatPKG>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "utils.h"
#include "blk_io.h"
#include "rvtimer.h"
#include "vector.h"
#include "spinlock.h"
#include "stacktrace.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * String/Integer conversions
 */

static inline char digit_symbol(uint32_t val)
{
    if (val < 10) return '0' + val;
    if (val < 36) return 'a' + val - 10;
    return '?';
}

static inline uint32_t digit_value(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'A' && digit <= 'Z') return digit - 'A' + 10;
    if (digit >= 'a' && digit <= 'z') return digit - 'a' + 10;
    return -1;
}

uint64_t uint_to_str_base(char* str, uint64_t size, uint64_t val, uint8_t base)
{
    uint64_t len = 0;
    if (base >= 2 && base <= 36) do {
        if (len + 1 >= size) {
            len = 0;
            break;
        }
        str[len++] = digit_symbol(val % base);
        val /= base;
    } while (val);
    // Reverse the string
    for (uint64_t i=0; i<len / 2; ++i) {
        char tmp = str[i];
        str[i] = str[len - i - 1];
        str[len - i - 1] = tmp;
    }
    if (size) str[len] = 0;
    return len;
}

uint64_t str_to_uint_base(const char* str, uint64_t* len, uint8_t base)
{
    uint64_t val = 0;
    uint64_t size = 0;
    if (base == 0) {
        base = 10;
        if (str[0] == '0') {
            base = 8; // Octal literal
            if (str[1] == 'o' || str[1] == 'O') {
                size = 2;
            } else if (str[1] == 'x' || str[1] == 'X') {
                base = 16; // Hex literal
                size = 2;
            } else if (str[1] == 'b' || str[1] == 'B') {
                base = 2; // Binary literal
                size = 2;
            }
        }
    }
    if (len) len[0] = 0;
    if (base >= 2 && base <= 36) while (digit_value(str[size]) < base) {
        val *= base;
        val += digit_value(str[size++]);
        if (len) len[0] = size;
    }
    return val;
}

uint64_t int_to_str_base(char* str, uint64_t size, int64_t val, uint8_t base)
{
    uint64_t off = (val < 0 && size) ? 1 : 0;
    uint64_t len = uint_to_str_base(str + off, size - off, off ? -val : val, base);
    if (!len) {
        if (size) str[0] = 0;
    } else if (off) {
        str[0] = '-';
        len += off;
    }
    return len;
}

int64_t str_to_int_base(const char* str, uint64_t* len, uint8_t base)
{
    bool neg = (str[0] == '-');
    uint64_t val = str_to_uint_base(str + neg, len, base);
    if (neg && len && len[0]) len[0]++;
    return neg ? -val : val;
}

uint64_t int_to_str_dec(char* str, uint64_t size, int64_t val)
{
    return int_to_str_base(str, size, val, 10);
}

int64_t str_to_int_dec(const char* str)
{
    return str_to_int_base(str, NULL, 0);
}

/*
 * String functions
 */

uint64_t rvvm_strlen(const char* string)
{
    uint64_t i = 0;
    while (string[i]) i++;
    return i;
}

uint64_t rvvm_strnlen(const char* string, uint64_t size)
{
    uint64_t i = 0;
    while (i < size && string[i]) i++;
    return i;
}

bool rvvm_strcmp(const char* s1, const char* s2)
{
    uint64_t i = 0;
    while (s1[i] && s1[i] == s2[i]) i++;
    return s1[i] == s2[i];
}

uint64_t rvvm_strlcpy(char* dst, const char* src, uint64_t size)
{
    uint64_t i = 0;
    while (i + 1 < size && src[i]) {
        dst[i] = src[i];
        i++;
    }
    if (size) dst[i] = 0;
    return i;
}

const char* rvvm_strfind(const char* string, const char* pattern)
{
    while (*string) {
        const char* tmp = string;
        const char* pat = pattern;
        while (*tmp && *tmp == *pat) {
            tmp++;
            pat++;
        }
        if (!(*pat)) return string;
        string++;
    }
    return NULL;
}

/*
 * Random generation
 */

void rvvm_randombytes(void* buffer, uint64_t size)
{
    // Xorshift RNG seeded by precise timer
    static uint64_t seed = 0;
    uint8_t* bytes = buffer;
    uint64_t size_rem = size & 0x7;
    size -= size_rem;
    seed += rvtimer_clocksource(1000000000ULL);
    for (uint64_t i=0; i<size; i += 8) {
        seed ^= (seed >> 17);
        seed ^= (seed << 21);
        seed ^= (seed << 28);
        seed ^= (seed >> 49);
        memcpy(bytes + i, &seed, 8);
    }
    seed ^= (seed >> 17);
    seed ^= (seed << 21);
    seed ^= (seed << 28);
    seed ^= (seed >> 49);
    memcpy(bytes + size, &seed, size_rem);
}

void rvvm_randomserial(char* serial, uint64_t size)
{
    rvvm_randombytes(serial, size);
    for (uint64_t i=0; i<size; ++i) {
        uint64_t c = ((uint8_t*)serial)[i] % ('Z' - 'A' + 10);
        if (c <= 9) serial[i] = '0' + c;
        else serial[i] = 'A' + c - 10;
    }
}

/*
 * Safe memory allocation
 */

SAFE_MALLOC void* safe_malloc(uint64_t size)
{
    void* ret = malloc(size);
    if (unlikely(!size)) rvvm_warn("Suspicious 0-byte allocation");
    if (unlikely(ret == NULL)) {
        rvvm_fatal("Out of memory!");
    }
    return ret;
}

SAFE_CALLOC void* safe_calloc(uint64_t size, uint64_t n)
{
    void* ret = calloc(size, n);
    if (unlikely(!size || !n)) rvvm_warn("Suspicious 0-byte allocation");
    if (unlikely(ret == NULL)) {
        rvvm_fatal("Out of memory!");
    }
    // Fence zeroing of allocated memory
    atomic_fence_ex(ATOMIC_RELEASE);
    return ret;
}

SAFE_REALLOC void* safe_realloc(void* ptr, uint64_t size)
{
    void* ret = realloc(ptr, size);
    if (unlikely(!size)) rvvm_warn("Suspicious 0-byte allocation");
    if (unlikely(ret == NULL)) {
        rvvm_fatal("Out of memory!");
    }
    return ret;
}

/*
 * Command line & config parsing
 */

static int    argc_internal = 0;
static char** argv_internal = NULL;

void rvvm_set_args(int argc, char** argv)
{
    argc_internal = argc;
    argv_internal = argv;

    if (rvvm_has_arg("v") || rvvm_has_arg("verbose")) {
        rvvm_set_loglevel(LOG_INFO);
    }
}

static bool config_skip(const char** strptr)
{
    bool found_newline = false;
    const char* str = *strptr;
    while (*str == ' ' || *str == '\n') {
        if (*str++ == '\n') {
            found_newline = true;
        }
    }
    if (*str == '#') {
        // Found a comment. skip till newline
        while (*str && *str != '\n') {
            str++;
        }
        *strptr = str;
        config_skip(strptr);
        return true;
    }
    *strptr = str;
    return found_newline;
}

static int config_split(const char* str, char** argv)
{
    int argc = 1;
    bool arg_prefix = true;
    if (argv) {
        argv[0] = "rvvm";
    }
    config_skip(&str);
    while (*str) {
        const char* arg_start = str;
        while (*str && *str != ' ' && *str != '\n') {
            str++;
        }
        if (argv) {
            uint64_t arg_size = str - arg_start;
            char* buffer = safe_new_arr(char, arg_size + (arg_prefix ? 2 : 1));
            if (arg_prefix) {
                buffer[0] = '-';
                memcpy(buffer + 1, arg_start, arg_size);
            } else {
                memcpy(buffer, arg_start, arg_size);
            }
            argv[argc] = buffer;
        }
        arg_prefix = config_skip(&str);
        argc++;
    }
    return argc;
}

bool rvvm_load_config(const char* path)
{
    rvfile_t* cfg = rvopen(path, 0);
    if (cfg) {
        uint64_t filesize = rvfilesize(cfg);
        char* filebuf = safe_new_arr(char, filesize + 1);
        rvread(cfg, filebuf, filesize, 0);
        rvclose(cfg);

        int argc = config_split(filebuf, NULL);
        char** argv = safe_new_arr(char*, argc + 1);
        config_split(filebuf, argv);
        free(filebuf);
        rvvm_set_args(argc, argv);
    }
    return false;
}

const char* rvvm_next_arg(const char** val, int* iter)
{
    int i = *iter;
    if (argc_internal < 2) {
        DO_ONCE(rvvm_load_config("rvvm.cfg"));
    }
    if (val) {
        *val = NULL;
    }
    if (argv_internal && i < argc_internal && argv_internal[i]) {
        const char* arg = argv_internal[i];
        if (arg[0] == '-') {
            // Skip -, -- argument prefix
            arg += (arg[1] == '-') ? 2 : 1;
            // Get trailing argument value, if any
            if (i + 1 < argc_internal) {
                const char* next_arg = argv_internal[i + 1];
                if (next_arg && next_arg[0] != '-') {
                    if (val) {
                        *val = next_arg;
                    }
                    i++;
                }
            }
        } else {
            // This is a free-standing value without -arg prefix
            if (val) {
                *val = arg;
            }
            arg = "";
        }
        *iter = i + 1;
        return arg;
    }
    return NULL;
}

bool rvvm_has_arg(const char* arg)
{
    const char* arg_name = NULL;
    int arg_iter = 1;
    while ((arg_name = rvvm_next_arg(NULL, &arg_iter))) {
        if (rvvm_strcmp(arg_name, arg)) {
            return true;
        }
    }
    return false;
}

const char* rvvm_getarg(const char* arg)
{
    const char* arg_name = NULL;
    const char* arg_val = NULL;
    int arg_iter = 1;
    while ((arg_name = rvvm_next_arg(&arg_val, &arg_iter))) {
        if (rvvm_strcmp(arg_name, arg)) {
            return arg_val;
        }
    }
    return NULL;
}

bool rvvm_getarg_bool(const char* arg)
{
    const char* arg_val = rvvm_getarg(arg);
    if (arg_val) {
        return rvvm_strcmp(arg_val, "true");
    }
    return false;
}

int rvvm_getarg_int(const char* arg)
{
    const char* arg_val = rvvm_getarg(arg);
    if (arg_val) {
        return str_to_int_dec(arg_val);
    }
    return 0;
}

uint64_t rvvm_getarg_size(const char* arg)
{
    const char* arg_val = rvvm_getarg(arg);
    if (arg_val) {
        uint64_t len = 0;
        uint64_t ret = str_to_int_base(arg_val, &len, 0);
        return ret << mem_suffix_shift(arg_val[len]);
    }
    return 0;
}

/*
 * Logger
 */

static int rvvm_loglevel = LOG_WARN;

void rvvm_set_loglevel(int loglevel)
{
    rvvm_loglevel = loglevel;
}

static bool log_has_colors(void)
{
    return getenv("TERM") != NULL;
}

static void log_print(const char* prefix, const char* fmt, va_list args)
{
    char buffer[256] = {0};
    uint64_t pos = rvvm_strlcpy(buffer, prefix, sizeof(buffer));
    uint64_t vsp_size = sizeof(buffer) - EVAL_MIN(pos + 6, sizeof(buffer));
    if (vsp_size > 1) {
        int tmp = vsnprintf(buffer + pos, vsp_size, fmt, args);
        if (tmp > 0) pos += EVAL_MIN(vsp_size - 1, (uint64_t)tmp);
    }
    rvvm_strlcpy(buffer + pos, log_has_colors() ? "\033[0m\n" : "\n", sizeof(buffer) - pos);
    fputs(buffer, stderr);
}

#ifdef USE_DEBUG

PRINT_FORMAT void rvvm_debug(const char* format_str, ...)
{
    if (rvvm_loglevel >= LOG_INFO) {
        va_list args;
        va_start(args, format_str);
        log_print(log_has_colors() ? "\033[33;1mDEBUG\033[37;1m: " : "DEBUG: ", format_str, args);
        va_end(args);
    }
}

#endif

PRINT_FORMAT void rvvm_info(const char* format_str, ...)
{
    if (rvvm_loglevel >= LOG_INFO) {
        va_list args;
        va_start(args, format_str);
        log_print(log_has_colors() ? "\033[33;1mINFO\033[37;1m: " : "INFO: ", format_str, args);
        va_end(args);
    }
}

PRINT_FORMAT void rvvm_warn(const char* format_str, ...)
{
    if (rvvm_loglevel >= LOG_WARN) {
        va_list args;
        va_start(args, format_str);
        log_print(log_has_colors() ? "\033[31;1mWARN\033[37;1m: " : "WARN: ", format_str, args);
        va_end(args);
    }
}

PRINT_FORMAT void rvvm_error(const char* format_str, ...)
{
    if (rvvm_loglevel >= LOG_ERROR) {
        va_list args;
        va_start(args, format_str);
        log_print(log_has_colors() ? "\033[31;1mERROR\033[37;1m: " : "ERROR: ", format_str, args);
        va_end(args);
    }
}

PRINT_FORMAT void rvvm_fatal(const char* format_str, ...)
{
    va_list args;
    va_start(args, format_str);
    log_print(log_has_colors() ? "\033[31;1mFATAL\033[37;1m: " : "FATAL: ", format_str, args);
    va_end(args);
    stacktrace_print();
    abort();
}

/*
 * Initialization/deinitialization
 */

slow_path void do_once_finalize(uint32_t* ticket)
{
    while (atomic_load_uint32_ex(ticket, ATOMIC_ACQUIRE) != 2) {
        sleep_ms(1);
    }
}

typedef void (*deinit_func_t)(void);
static vector_t(deinit_func_t) deinit_funcs = {0};
static spinlock_t deinit_lock = {0};
static bool deinit_happened = false;

void call_at_deinit(void (*function)(void))
{
    bool call_func = false;

    while (!spin_try_lock(&deinit_lock)) sleep_ms(1);
    if (!deinit_happened) {
        vector_push_back(deinit_funcs, function);
    } else {
        call_func = true;
    }
    spin_unlock(&deinit_lock);

    if (call_func) {
        function();
    }
}

static deinit_func_t dequeue_func(void)
{
    deinit_func_t ret = NULL;

    while (!spin_try_lock(&deinit_lock)) sleep_ms(1);
    deinit_happened = true;
    if (vector_size(deinit_funcs) == 0) {
        vector_free(deinit_funcs);
    } else {
        uint64_t end = vector_size(deinit_funcs) - 1;
        ret = vector_at(deinit_funcs, end);
        vector_erase(deinit_funcs, end);
    }
    spin_unlock(&deinit_lock);
    return ret;
}

GNU_DESTRUCTOR void full_deinit(void)
{
    rvvm_info("Fully deinitializing librvvm");
    while (true) {
        deinit_func_t func = dequeue_func();
        if (func) {
            func();
        } else {
            break;
        }
    }
}

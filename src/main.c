/*
main.c - RVVM CLI, API usage example
Copyright (C) 2021  LekKit <github.com/LekKit>
                    cerg2010cerg2010 <github.com/cerg2010cerg2010>
                    Mr0maks <mr.maks0443@gmail.com>
                    KotB <github.com/0xCatPKG>
                    fish4terrisa-MSDSM <fish4terrisa@fishinix.eu.org>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "rvvmlib.h"
#include "rvvm_user.h"
#include "rvvm_isolation.h"
#include "utils.h"
#include "dlib.h"
#include "gdbstub.h"

#include "devices/riscv-imsic.h"
#include "devices/riscv-aplic.h"
#include "devices/riscv-aclint.h"
#include "devices/riscv-plic.h"
#include "devices/syscon.h"
#include "devices/rtc-goldfish.h"
#include "devices/ns16550a.h"
#include "devices/gui_window.h"
#include "devices/pci-bus.h"
#include "devices/pci-vfio.h"
#include "devices/nvme.h"
#include "devices/ata.h"
#include "devices/rtl8169.h"
#include "devices/i2c-oc.h"
#include "devices/usb-xhci.h"

#include <stdio.h>
#include <inttypes.h>

#ifdef _WIN32
// For unicode args/console
#include <windows.h>
#endif

static void rvvm_print_help(void)
{
#if defined(_WIN32) && !defined(UNDER_CE)
    const wchar_t* help = L"\n"
#else
    printf("\n"
#endif
           "  ██▀███   ██▒   █▓ ██▒   █▓ ███▄ ▄███▓\n"
           " ▓██ ▒ ██▒▓██░   █▒▓██░   █▒▓██▒▀█▀ ██▒\n"
           " ▓██ ░▄█ ▒ ▓██  █▒░ ▓██  █▒░▓██    ▓██░\n"
           " ▒██▀▀█▄    ▒██ █░░  ▒██ █░░▒██    ▒██ \n"
           " ░██▓ ▒██▒   ▒▀█░     ▒▀█░  ▒██▒   ░██▒\n"
           " ░ ▒▓ ░▒▓░   ░ ▐░     ░ ▐░  ░ ▒░   ░  ░\n"
           "   ░▒ ░ ▒░   ░ ░░     ░ ░░  ░  ░      ░\n"
           "   ░░   ░      ░░       ░░  ░      ░   \n"
           "    ░           ░        ░         ░   \n"
           "               ░        ░              \n"
           "\n"
           "https://github.com/LekKit/RVVM (v"RVVM_VERSION")\n"
           "\n"
           "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n"
           "This is free software: you are free to change and redistribute it.\n"
           "There is NO WARRANTY, to the extent permitted by law.\n"
           "\n"
           "Usage: rvvm <firmware> [-m 256M] [-k kernel] [-i drive.img] ...\n"
           "\n"
           "    <firmware>       Initial M-mode firmware (OpenSBI [+ U-Boot], etc)\n"
           "    -k, -kernel ...  Optional S-mode kernel payload (Linux, U-Boot, etc)\n"
           "    -i, -image  ...  Attach preferred storage image (Currently as NVMe)\n"
           "    -m, -mem 1G      Memory amount, default: 256M\n"
           "    -s, -smp 4       Cores count, default: 1\n"
           "    -rv32            Enable 32-bit RISC-V, 64-bit by default\n"
           "    -cmdline    ...  Override payload kernel command line\n"
           "    -append     ...  Modify payload kernel command line\n"
           "    -res 1280x720    Set display(s) resolution\n"
           "    -poweroff_key    Send HID_KEY_POWER instead of exiting on GUI close\n"
           "    -portfwd 8080=80 Port forwarding (Extended: tcp/127.0.0.1:8080=80)\n"
           "    -vfio_pci   ...  PCI passthrough via VFIO (Example: 00:02.0), needs root\n"
           "    -nvme       ...  Explicitly attach storage image as NVMe device\n"
           "    -ata        ...  Explicitly attach storage image as ATA (IDE) device\n"
           "    -nogui           Disable display GUI\n"
           "    -nonet           Disable networking\n"
           "    -serial     ...  Add more serial ports (Via pty/pipe path), or null\n"
           "    -dtb        ...  Pass custom Device Tree Blob to the machine\n"
           "    -dumpdtb    ...  Dump auto-generated DTB to file\n"
           "    -v, -verbose     Enable verbose logging\n"
           "    -h, -help        Show this help message\n"
           "\n"
           "    -noisolation     Disable seccomp/pledge isolation\n"
           "    -nojit           Disable RVJIT (For debug purposes, slow!)\n"
#if defined(_WIN32) && !defined(UNDER_CE)
           "\n";
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), help, wcslen(help), NULL, NULL);
#else
           "\n");
#endif
}

static bool rvvm_cli_configure(rvvm_machine_t* machine, const char* bios, tap_dev_t* tap)
{
    UNUSED(tap);

    if (rvvm_has_arg("k") || rvvm_has_arg("kernel")) {
        // If we are booting a static kernel, append root device cmdline
        rvvm_append_cmdline(machine, "root=/dev/nvme0n1 rootflags=discard rw");
    }
    if (!rvvm_load_bootrom(machine, bios)) {
        return false;
    }
    if (rvvm_getarg("k") && !rvvm_load_kernel(machine, rvvm_getarg("k"))) {
        return false;
    }
    if (rvvm_getarg("kernel") && !rvvm_load_kernel(machine, rvvm_getarg("kernel"))) {
        return false;
    }
    if (rvvm_getarg("dtb") && !rvvm_load_dtb(machine, rvvm_getarg("dtb"))) {
        return false;
    }

    int arg_iter = 1;
    const char* arg_name = NULL;
    const char* arg_val = NULL;
    while ((arg_name = rvvm_next_arg(&arg_val, &arg_iter))) {
        if (arg_val) {
            if (rvvm_strcmp(arg_name, "i") || rvvm_strcmp(arg_name, "image") || rvvm_strcmp(arg_name, "nvme")) {
                if (!nvme_init_auto(machine, arg_val, true)) {
                    rvvm_error("Failed to attach image \"%s\"", arg_val);
                    return false;
                }
            } else if (rvvm_strcmp(arg_name, "ata")) {
                if (!ata_init_auto(machine, arg_val, true)) {
                    rvvm_error("Failed to attach image \"%s\"", arg_val);
                    return false;
                }
            } else if (rvvm_strcmp(arg_name, "serial")) {
                chardev_t* chardev = chardev_pty_create(arg_val);
                if (chardev == NULL && !rvvm_strcmp(arg_val, "null")) {
                    return false;
                }
                ns16550a_init_auto(machine, chardev);
            } else if (rvvm_strcmp(arg_name, "res")) {
                uint64_t len = 0;
                uint32_t fb_x = str_to_uint_base(arg_val, &len, 10);
                uint32_t fb_y = str_to_uint_base(arg_val + len + 1, NULL, 10);
                if (arg_val[len] != 'x') fb_y = 0;
                if (fb_x < 100 || fb_y < 100) {
                    rvvm_error("Invalid resoulution: %s, expects 640x480", arg_val);
                    return false;
                }
                gui_window_init_auto(machine, fb_x, fb_y);
#ifdef USE_NET
            } else if (tap && rvvm_strcmp(arg_name, "portfwd")) {
                if (!tap_portfwd(tap, arg_val)) {
                    return false;
                }
#endif
            } else if (rvvm_strcmp(arg_name, "vfio_pci")) {
                if (!pci_vfio_init_auto(machine, arg_val)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static int rvvm_cli_main(int argc, char** argv)
{
    // Set up argparser
    rvvm_set_args(argc, argv);
    if (rvvm_has_arg("h") || rvvm_has_arg("help") || rvvm_has_arg("H")) {
        rvvm_print_help();
        return 0;
    }

    // Default machine parameters: 1 core, 256M ram, riscv64, 640x480 screen
    uint64_t    mem = rvvm_getarg_size("m");
    if (!mem) mem = rvvm_getarg_size("mem");
    if (!mem) mem = (256 << 20);

    uint64_t    smp = rvvm_getarg_int("s");
    if (!smp) smp = rvvm_getarg_int("smp");
    if (!smp) smp = 1;

    const char* bios = rvvm_getarg("");
    if (!bios)  bios = rvvm_getarg("bios");

    const char* isa = rvvm_getarg("isa");
    if (!isa)   isa = rvvm_has_arg("rv32") ? "rv32" : "rv64";

    if (!bios) {
        // No firmware passed
        printf("Usage: rvvm <firmware> [-mem 256M] [-k kernel] [-help] ...\n");
        return 0;
    }

    // Create the VM with specified architecture, amount of RAM / cores
    rvvm_machine_t* machine = rvvm_create_machine(mem, smp, isa);
    if (machine == NULL) {
        rvvm_error("Failed to create VM");
        return -1;
    }

    // Initialize basic peripherals (Interrupt controllers, PCI bus, clocks, power management)
    riscv_clint_init_auto(machine);

    if (rvvm_has_arg("riscv_aia")) {
        // Use RISC-V Advanced Interrupt Architecture (IMSIC + APLIC)
        riscv_imsic_init_auto(machine);
        riscv_aplic_init_auto(machine);
    } else {
        // Use SiFive PLIC
        riscv_plic_init_auto(machine);
    }

    pci_bus_init_auto(machine);
    i2c_oc_init_auto(machine);

    //usb_xhci_init(rvvm_get_pci_bus(machine));

    rtc_goldfish_init_auto(machine);
    syscon_init_auto(machine);

    if (rvvm_has_arg("gdbstub")) {
        gdbstub_init(machine, rvvm_getarg("gdbstub"));
    }

    if (!rvvm_has_arg("serial")) {
        ns16550a_init_term_auto(machine);
    }

    if (!rvvm_has_arg("nogui") && !rvvm_has_arg("res")) {
        gui_window_init_auto(machine, 640, 480);
    }

    tap_dev_t* tap = NULL;
#ifdef USE_NET
    if (!rvvm_has_arg("nonet")) {
        tap = tap_open();
        rtl8169_init(rvvm_get_pci_bus(machine), tap);
    }
#endif

    if (!rvvm_cli_configure(machine, bios, tap)) {
        rvvm_error("Failed to initialize VM");
        rvvm_free_machine(machine);
        return -1;
    }

    if (rvvm_getarg("cmdline")) {
        rvvm_set_cmdline(machine, rvvm_getarg("cmdline"));
    }
    if (rvvm_getarg("append")) {
        rvvm_append_cmdline(machine, rvvm_getarg("append"));
    }
    if (rvvm_getarg("dumpdtb")) {
        // Dump the machine DTB after it's been fully configured
        rvvm_dump_dtb(machine, rvvm_getarg("dumpdtb"));
    }

    rvvm_start_machine(machine);

    if (!rvvm_has_arg("noisolation")) {
        // Preparations are done, isolate the process as much as possible
        rvvm_restrict_process();
    }

    // Returns on machine shutdown
    rvvm_run_eventloop();

    rvvm_free_machine(machine);
    return 0;
}

int main(int argc, char** argv)
{
#if defined(_WIN32) && !defined(UNDER_CE)
    // Use UTF-8 arguments
    LPWSTR* argv_u16 = CommandLineToArgvW(GetCommandLineW(), &argc);
    argv = safe_new_arr(char*, argc + 1);
    for (int i=0; i<argc; ++i) {
        uint64_t arg_len = WideCharToMultiByte(CP_UTF8, 0, argv_u16[i], -1, NULL, 0, NULL, NULL);
        argv[i] = safe_new_arr(char, arg_len);
        WideCharToMultiByte(CP_UTF8, 0, argv_u16[i], -1, argv[i], arg_len, NULL, NULL);
    }
    LocalFree(argv_u16);
    int ret = rvvm_cli_main(argc, argv);
    for (int i=0; i<argc; ++i) {
        free(argv[i]);
    }
    free(argv);
    return ret;
#else
    return rvvm_cli_main(argc, argv);
#endif
}

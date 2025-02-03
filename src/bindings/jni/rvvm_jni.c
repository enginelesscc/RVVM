#include "tiny-jni.h"
#include "compiler.h"
#include "utils.h"
#include "vma_ops.h"

#include "rvvmlib.h"

#include "devices/riscv-aclint.h"
#include "devices/riscv-imsic.h"
#include "devices/riscv-plic.h"
#include "devices/riscv-aplic.h"

#include "devices/pci-bus.h"
#include "devices/i2c-oc.h"

#include "devices/syscon.h"
#include "devices/rtc-goldfish.h"
#include "devices/rtc-ds1742.h"
#include "devices/ns16550a.h"
#include "devices/mtd-physmap.h"
#include "devices/framebuffer.h"

#include "devices/rtl8169.h"
#include "devices/nvme.h"

#include "devices/hid_api.h"
#include "devices/gpio-sifive.h"

PUSH_OPTIMIZATION_SIZE

JNIEXPORT jboolean JNICALL Java_lekkit_rvvm_RVVMNative_check_1abi(JNIEnv* env, jclass class, jint abi)
{
    UNUSED(env); UNUSED(class);
    return rvvm_check_abi(abi);
}

/*
 * RVVM Machine Management API
 */

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_create_1machine(JNIEnv* env, jclass class, jlong mem_size, jint smp, jstring isa)
{
    const char* u8_isa = (*env)->GetStringUTFChars(env, isa, NULL);
    UNUSED(class);
    jlong ret = (uint64_t)rvvm_create_machine(mem_size, smp, u8_isa);
    (*env)->ReleaseStringUTFChars(env, isa, u8_isa);
    return ret;
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_set_1cmdline(JNIEnv* env, jclass class, jlong machine, jstring cmdline)
{
    const char* u8_cmdline = (*env)->GetStringUTFChars(env, cmdline, NULL);
    UNUSED(class);
    rvvm_set_cmdline((rvvm_machine_t*)(uint64_t)machine, u8_cmdline);
    (*env)->ReleaseStringUTFChars(env, cmdline, u8_cmdline);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_append_1cmdline(JNIEnv* env, jclass class, jlong machine, jstring cmdline)
{
    const char* u8_cmdline = (*env)->GetStringUTFChars(env, cmdline, NULL);
    UNUSED(class);
    rvvm_append_cmdline((rvvm_machine_t*)(uint64_t)machine, u8_cmdline);
    (*env)->ReleaseStringUTFChars(env, cmdline, u8_cmdline);
}

JNIEXPORT jboolean JNICALL Java_lekkit_rvvm_RVVMNative_load_1bootrom(JNIEnv* env, jclass class, jlong machine, jstring path)
{
    const char* u8_path = (*env)->GetStringUTFChars(env, path, NULL);
    bool ret = rvvm_load_bootrom((rvvm_machine_t*)(uint64_t)machine, u8_path);
    UNUSED(class);
    (*env)->ReleaseStringUTFChars(env, path, u8_path);
    return ret;
}

JNIEXPORT jboolean JNICALL Java_lekkit_rvvm_RVVMNative_load_1kernel(JNIEnv* env, jclass class, jlong machine, jstring path)
{
    const char* u8_path = (*env)->GetStringUTFChars(env, path, NULL);
    bool ret = rvvm_load_kernel((rvvm_machine_t*)(uint64_t)machine, u8_path);
    UNUSED(class);
    (*env)->ReleaseStringUTFChars(env, path, u8_path);
    return ret;
}

JNIEXPORT jboolean JNICALL Java_lekkit_rvvm_RVVMNative_load_1dtb(JNIEnv* env, jclass class, jlong machine, jstring path)
{
    const char* u8_path = (*env)->GetStringUTFChars(env, path, NULL);
    bool ret = rvvm_load_dtb((rvvm_machine_t*)(uint64_t)machine, u8_path);
    UNUSED(class);
    (*env)->ReleaseStringUTFChars(env, path, u8_path);
    return ret;
}

JNIEXPORT jboolean JNICALL Java_lekkit_rvvm_RVVMNative_dump_1dtb(JNIEnv* env, jclass class, jlong machine, jstring path)
{
    const char* u8_path = (*env)->GetStringUTFChars(env, path, NULL);
    bool ret = rvvm_dump_dtb((rvvm_machine_t*)(uint64_t)machine, u8_path);
    UNUSED(class);
    (*env)->ReleaseStringUTFChars(env, path, u8_path);
    return ret;
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_get_1opt(JNIEnv* env, jclass class, jlong machine, jint opt)
{
    UNUSED(env); UNUSED(class);
    return rvvm_get_opt((rvvm_machine_t*)(uint64_t)machine, (uint32_t)opt);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_set_1opt(JNIEnv* env, jclass class, jlong machine, jint opt, jlong val)
{
    UNUSED(env); UNUSED(class);
    rvvm_set_opt((rvvm_machine_t*)(uint64_t)machine, (uint32_t)opt, val);
}

JNIEXPORT jboolean JNICALL Java_lekkit_rvvm_RVVMNative_start_1machine(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return rvvm_start_machine((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jboolean JNICALL Java_lekkit_rvvm_RVVMNative_pause_1machine(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return rvvm_pause_machine((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jboolean JNICALL Java_lekkit_rvvm_RVVMNative_reset_1machine(JNIEnv* env, jclass class, jlong machine, jboolean reset)
{
    UNUSED(env); UNUSED(class);
    rvvm_reset_machine((rvvm_machine_t*)(uint64_t)machine, reset);
    return true;
}

JNIEXPORT jboolean JNICALL Java_lekkit_rvvm_RVVMNative_machine_1running(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return rvvm_machine_running((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jboolean JNICALL Java_lekkit_rvvm_RVVMNative_machine_1powered(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return rvvm_machine_powered((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_free_1machine(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    rvvm_free_machine((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_run_1eventloop(JNIEnv* env, jclass class)
{
    UNUSED(env); UNUSED(class);
    rvvm_run_eventloop();
}

/*
 * RVVM Device API
 */

JNIEXPORT jobject JNICALL Java_lekkit_rvvm_RVVMNative_get_1dma_1buf(JNIEnv* env, jclass class, jlong machine, jlong addr, jlong size)
{
    void* ptr = rvvm_get_dma_ptr((rvvm_machine_t*)(uint64_t)machine, addr, size);
    UNUSED(class);
    if (ptr == NULL) return NULL;
    return (*env)->NewDirectByteBuffer(env, ptr, size);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_mmio_1zone_1auto(JNIEnv* env, jclass class, jlong machine, jlong addr, jlong size)
{
    UNUSED(env); UNUSED(class);
    return rvvm_mmio_zone_auto((rvvm_machine_t*)(uint64_t)machine, addr, size);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_remove_1mmio(JNIEnv* env, jclass class, jlong mmio_dev)
{
    UNUSED(env); UNUSED(class);
    rvvm_remove_mmio((rvvm_mmio_dev_t*)(uint64_t)mmio_dev);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_get_1intc(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)rvvm_get_intc((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_set_1intc(JNIEnv* env, jclass class, jlong machine, jlong intc)
{
    UNUSED(env); UNUSED(class);
    rvvm_set_intc((rvvm_machine_t*)(uint64_t)machine, (rvvm_intc_t*)(uint64_t)intc);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_get_1pci_1bus(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)rvvm_get_pci_bus((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_set_1pci_1bus(JNIEnv* env, jclass class, jlong machine, jlong pci_bus)
{
    UNUSED(env); UNUSED(class);
    rvvm_set_pci_bus((rvvm_machine_t*)(uint64_t)machine, (pci_bus_t*)(uint64_t)pci_bus);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_get_1i2c_1bus(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)rvvm_get_i2c_bus((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_set_1i2c_1bus(JNIEnv* env, jclass class, jlong machine, jlong i2c_bus)
{
    UNUSED(env); UNUSED(class);
    rvvm_set_i2c_bus((rvvm_machine_t*)(uint64_t)machine, (i2c_bus_t*)(uint64_t)i2c_bus);
}

/*
 * RVVM Devices
 */

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_riscv_1clint_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    riscv_clint_init_auto((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_riscv_1imsic_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    riscv_imsic_init_auto((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_riscv_1plic_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)riscv_plic_init_auto((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_riscv_1aplic_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)riscv_aplic_init_auto((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_pci_1bus_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)pci_bus_init_auto((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_i2c_1bus_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)i2c_oc_init_auto((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_tap_1user_1open(JNIEnv* env, jclass class)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)tap_open();
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_syscon_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)syscon_init_auto((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_rtc_1goldfish_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)rtc_goldfish_init_auto((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_rtc_1ds1742_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)rtc_ds1742_init_auto((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_ns16550a_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)ns16550a_init_term_auto((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_gpio_1sifive_1init_1auto(JNIEnv* env, jclass class, jlong machine, jlong gpio)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)gpio_sifive_init_auto((rvvm_machine_t*)(uint64_t)machine, (rvvm_gpio_dev_t*)(uint64_t)gpio);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_mtd_1physmap_1init_1auto(JNIEnv* env, jclass class, jlong machine, jstring path, jboolean rw)
{
    const char* u8_path = (*env)->GetStringUTFChars(env, path, NULL);
    rvvm_mmio_dev_t* mmio = mtd_physmap_init_auto((rvvm_machine_t*)(uint64_t)machine, u8_path, rw);
    UNUSED(class);
    (*env)->ReleaseStringUTFChars(env, path, u8_path);
    return (uint64_t)mmio;
}

static void jni_framebuffer_remove(rvvm_mmio_dev_t* dev)
{
    if (dev->mapping && dev->size) {
        vma_free(dev->mapping, dev->size);
    }
}

static const rvvm_mmio_type_t jni_framebuffer_dev_type = {
    .name = "framebuffer",
    .remove = jni_framebuffer_remove,
};

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_framebuffer_1init_1auto(JNIEnv* env, jclass class, jlong machine, jobjectArray fb, jint x, jint y, jint bpp)
{
    fb_ctx_t fb_ctx = {
        .format = rgb_format_from_bpp(bpp),
        .width = x,
        .height = y,
    };
    UNUSED(class);

    if (!framebuffer_size(&fb_ctx)) {
        rvvm_warn("Invalid framebuffer size/bpp!");
        return 0;
    }

    fb_ctx.buffer = vma_alloc(NULL, framebuffer_size(&fb_ctx), VMA_RDWR);
    if (!fb_ctx.buffer) {
        rvvm_warn("Failed to allocate framebuffer via vma_alloc()!");
        return 0;
    }

    rvvm_mmio_dev_t* mmio = framebuffer_init_auto((rvvm_machine_t*)(uint64_t)machine, &fb_ctx);
    if (mmio) {
        // Return direct ByteBuffer to Java side, register framebuffer cleanup callback
        jobject bytebuf = (*env)->NewDirectByteBuffer(env, fb_ctx.buffer, framebuffer_size(&fb_ctx));
        mmio->type = &jni_framebuffer_dev_type;

        if (!bytebuf) {
            rvvm_warn("Failed to create direct ByteBuffer for framebuffer!");
            return 0;
        }

        (*env)->SetObjectArrayElement(env, fb, 0, bytebuf);
        return (uint64_t)mmio;
    }

    return 0;
}


JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_rtl8169_1init(JNIEnv* env, jclass class, jlong pci_bus, jlong tap)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)rtl8169_init((pci_bus_t*)(uint64_t)pci_bus, (tap_dev_t*)(uint64_t)tap);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_nvme_1init(JNIEnv* env, jclass class, jlong pci_bus, jstring path, jboolean rw)
{
    const char* u8_path = (*env)->GetStringUTFChars(env, path, NULL);
    pci_dev_t* ret = nvme_init((pci_bus_t*)(uint64_t)pci_bus, u8_path, rw);
    UNUSED(class);
    (*env)->ReleaseStringUTFChars(env, path, u8_path);
    return (uint64_t)ret;
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_hid_1mouse_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)hid_mouse_init_auto((rvvm_machine_t*)(uint64_t)machine);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_hid_1keyboard_1init_1auto(JNIEnv* env, jclass class, jlong machine)
{
    UNUSED(env); UNUSED(class);
    return (uint64_t)hid_keyboard_init_auto((rvvm_machine_t*)(uint64_t)machine);
}

static void jni_gpio_remove(rvvm_gpio_dev_t* gpio)
{
    free(gpio);
}

JNIEXPORT jlong JNICALL Java_lekkit_rvvm_RVVMNative_gpio_1dev_1create(JNIEnv* env, jclass class)
{
    rvvm_gpio_dev_t* gpio = safe_new_obj(rvvm_gpio_dev_t);
    gpio->remove = jni_gpio_remove;
    UNUSED(env); UNUSED(class);
    return (uint64_t)gpio;
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_pci_1remove_1device(JNIEnv* env, jclass class, jlong pci_dev)
{
    UNUSED(env); UNUSED(class);
    pci_remove_device((pci_dev_t*)(uint64_t)pci_dev);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_gpio_1dev_1free(JNIEnv* env, jclass class, jlong gpio)
{
    void* ptr = (void*)(uint64_t)gpio;
    UNUSED(env); UNUSED(class);
    free(ptr);
}

JNIEXPORT jint JNICALL Java_lekkit_rvvm_RVVMNative_gpio_1read_1pins(JNIEnv* env, jclass class, jlong gpio, jint off)
{
    UNUSED(env); UNUSED(class);
    return gpio_read_pins((rvvm_gpio_dev_t*)(uint64_t)gpio, off);
}

JNIEXPORT jboolean JNICALL Java_lekkit_rvvm_RVVMNative_gpio_1write_1pins(JNIEnv* env, jclass class, jlong gpio, jint off, jint pins)
{
    UNUSED(env); UNUSED(class);
    return gpio_write_pins((rvvm_gpio_dev_t*)(uint64_t)gpio, off, pins);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_hid_1mouse_1resolution(JNIEnv* env, jclass class, jlong mice, jint x, jint y)
{
    UNUSED(env); UNUSED(class);
    hid_mouse_resolution((hid_mouse_t*)(uint64_t)mice, x, y);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_hid_1mouse_1place(JNIEnv* env, jclass class, jlong mice, jint x, jint y)
{
    UNUSED(env); UNUSED(class);
    hid_mouse_place((hid_mouse_t*)(uint64_t)mice, x, y);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_hid_1mouse_1move(JNIEnv* env, jclass class, jlong mice, jint x, jint y)
{
    UNUSED(env); UNUSED(class);
    hid_mouse_move((hid_mouse_t*)(uint64_t)mice, x, y);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_hid_1mouse_1press(JNIEnv* env, jclass class, jlong mice, jbyte btns)
{
    UNUSED(env); UNUSED(class);
    hid_mouse_press((hid_mouse_t*)(uint64_t)mice, btns);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_hid_1mouse_1release(JNIEnv* env, jclass class, jlong mice, jbyte btns)
{
    UNUSED(env); UNUSED(class);
    hid_mouse_release((hid_mouse_t*)(uint64_t)mice, btns);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_hid_1mouse_1scroll(JNIEnv* env, jclass class, jlong mice, jint offset)
{
    UNUSED(env); UNUSED(class);
    hid_mouse_scroll((hid_mouse_t*)(uint64_t)mice, offset);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_hid_1keyboard_1press(JNIEnv* env, jclass class, jlong kb, jbyte key)
{
    UNUSED(env); UNUSED(class);
    hid_keyboard_press((hid_keyboard_t*)(uint64_t)kb, key);
}

JNIEXPORT void JNICALL Java_lekkit_rvvm_RVVMNative_hid_1keyboard_1release(JNIEnv* env, jclass class, jlong kb, jbyte key)
{
    UNUSED(env); UNUSED(class);
    hid_keyboard_release((hid_keyboard_t*)(uint64_t)kb, key);
}

POP_OPTIMIZATION_SIZE

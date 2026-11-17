#undef NDEBUG
#define usb_ohci_init test_usb_ohci_init
#define usb_ohci_attach_hid test_usb_ohci_attach_hid
#define rvvm_pci_get_dma_ex test_pci_get_dma_ex
#define rvvm_pci_end_dma test_pci_end_dma
#define rvvm_pci_set_irq test_pci_set_irq
#include "../src/devices/usb-ohci.c"
#include <assert.h>
#include <stdio.h>

static uint8_t memory[4096];
static bool check_periodic_read;

void* test_pci_get_dma_ex(rvvm_pci_func_t* pci, rvvm_addr_t addr, size_t* size, uint32_t attr)
{
    UNUSED(pci);
    UNUSED(attr);
    if (addr > sizeof(memory) || *size > sizeof(memory) - addr) return NULL;
    if (check_periodic_read && addr == 0x100 + 4 * 31) {
        assert(read_uint32_le(memory + 0x180) == 0xffff);
        check_periodic_read = false;
    }
    return memory + addr;
}

void test_pci_end_dma(rvvm_pci_func_t* pci, void* ptr)
{
    UNUSED(pci);
    UNUSED(ptr);
}

void test_pci_set_irq(rvvm_pci_func_t* pci, uint32_t vec, bool level)
{
    UNUSED(pci);
    UNUSED(vec);
    UNUSED(level);
}

int main(int argc, char** argv)
{
    usb_ohci_t hc = {0};
    hc.hcca = 0x100;
    bool ordering = argc > 1 && strcmp(argv[1], "ordering") == 0;
    hc.control = ordering ? 0x84 : 0x80;
    hc.frame = 0xfffe;
    memset(memory + 0x180, 0xaa, 4);
    check_periodic_read = ordering;
    frame(&hc);
    assert(!check_periodic_read);
    assert(read_uint32_le(memory + 0x180) == 0xffff);
    memset(memory + 0x182, 0xaa, 2);
    frame(&hc);
    assert(read_uint32_le(memory + 0x180) == 0);
    hc.control = 0;
    memset(memory + 0x180, 0xaa, 4);
    frame(&hc);
    assert(read_uint32_le(memory + 0x180) == 0xaaaaaaaa);
    puts("OHCI HCCA frame update passed");
    return 0;
}
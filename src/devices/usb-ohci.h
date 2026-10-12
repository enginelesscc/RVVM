#ifndef RVVM_USB_OHCI_H
#define RVVM_USB_OHCI_H
#include "hid_api.h"
#include "hid_dev.h"
typedef struct usb_ohci usb_ohci_t;
usb_ohci_t*             usb_ohci_init(rvvm_machine_t* machine);
void                    usb_ohci_attach_hid(usb_ohci_t* hc, hid_dev_t* hid, uint8_t protocol);
hid_keyboard_t*         hid_keyboard_init_usb(usb_ohci_t* hc);
hid_mouse_t*            hid_mouse_init_usb(usb_ohci_t* hc);
#endif

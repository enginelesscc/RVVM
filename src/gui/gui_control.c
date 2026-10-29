/*
 * Local framebuffer and HID control for automated guest testing.
 * SPDX-License-Identifier: MPL-2.0
 */

#include "gui_control.h"

#if defined(USE_GUI) && defined(USE_NET)

#include <util/networking.h>
#include <util/rvtimer.h>
#include <stdio.h>
#include <string.h>

struct gui_control {
    gui_window_t* win;
    hid_keyboard_t* keyboard;
    hid_mouse_t* mouse;
    net_sock_t* listener;
    net_sock_t* client;
    char request[256];
    size_t received;
    uint8_t* response;
    size_t length;
    size_t sent;
    uint64_t activity;
    bool keys[256];
    uint8_t buttons;
    bool closing;
};

static void gui_control_release(gui_control_t* control)
{
    for (unsigned key = 0; key < 256; ++key) {
        if (control->keys[key]) {
            hid_keyboard_release(control->keyboard, key);
            control->keys[key] = false;
        }
    }
    hid_mouse_release(control->mouse, control->buttons);
    control->buttons = 0;
}

static void gui_control_disconnect(gui_control_t* control)
{
    gui_control_release(control);
    net_sock_close(control->client);
    control->client = NULL;
    safe_free(control->response);
    control->received = control->length = control->sent = 0;
    control->closing = false;
}

static void gui_control_reply(gui_control_t* control, const char* message)
{
    control->length = strlen(message);
    control->response = safe_malloc(control->length);
    memcpy(control->response, message, control->length);
}

static void gui_control_frame(gui_control_t* control)
{
    rvvm_fb_t fb = ZERO_INIT;
    char header[128];
    rvvm_fbdev_get_scanout(control->win->fbdev, &fb);
    size_t size = rvvm_fb_size(&fb);
    if (!fb.buffer || !size || size > 64 * 1024 * 1024) {
        gui_control_reply(control, "ERROR framebuffer unavailable\n");
        return;
    }
    int count = snprintf(header, sizeof(header), "FRAME %u %u %u %u %u\n",
                         fb.width, fb.height, (unsigned)rvvm_fb_stride(&fb),
                         (unsigned)fb.format, (unsigned)size);
    control->length = count + size;
    control->response = safe_malloc(control->length);
    memcpy(control->response, header, count);
    memcpy(control->response + count, fb.buffer, size);
}

static void gui_control_command(gui_control_t* control)
{
    unsigned key, pressed, buttons;
    int x, y, wheel;
    char extra;
    if (rvvm_strcmp(control->request, "frame")) {
        gui_control_frame(control);
        return;
    } else if (sscanf(control->request, "key %3u %1u %c", &key, &pressed, &extra) == 2
               && key >= 4 && key < 256 && pressed <= 1) {
        if (pressed) {
            hid_keyboard_press(control->keyboard, key);
        } else {
            hid_keyboard_release(control->keyboard, key);
        }
        control->keys[key] = pressed;
    } else if (sscanf(control->request, "mouse %6d %6d %1u %c", &x, &y, &buttons, &extra) == 3
               && buttons <= 7) {
        rvvm_fb_t fb = ZERO_INIT;
        rvvm_fbdev_get_scanout(control->win->fbdev, &fb);
        if (x < 0 || y < 0 || (unsigned)x >= fb.width || (unsigned)y >= fb.height) {
            gui_control_reply(control, "ERROR coordinates outside framebuffer\n");
            return;
        }
        hid_mouse_resolution(control->mouse, fb.width, fb.height);
        hid_mouse_place(control->mouse, x, y);
        hid_mouse_release(control->mouse, control->buttons & ~buttons);
        hid_mouse_press(control->mouse, buttons & ~control->buttons);
        control->buttons = buttons;
    } else if (sscanf(control->request, "scroll %4d %c", &wheel, &extra) == 1
               && wheel >= -127 && wheel <= 127) {
        hid_mouse_scroll(control->mouse, wheel);
    } else if (rvvm_strcmp(control->request, "release")) {
        gui_control_release(control);
    } else if (rvvm_strcmp(control->request, "close")) {
        gui_control_release(control);
        control->closing = true;
    } else {
        gui_control_reply(control, "ERROR invalid command\n");
        return;
    }
    gui_control_reply(control, "OK\n");
}

gui_control_t* gui_control_init(gui_window_t* win, hid_keyboard_t* keyboard, hid_mouse_t* mouse)
{
    const char* option = rvvm_getarg("control");
    if (!option) {
        if (rvvm_has_arg("control")) {
            rvvm_fatal("-control requires a port");
        }
        return NULL;
    }
    size_t parsed = 0;
    uint64_t port = str_to_uint_base(option, &parsed, 10);
    if (!parsed || parsed > 5 || option[parsed] || port > 65535 || !port) {
        rvvm_fatal("Invalid -control port");
    }
    gui_control_t* control = safe_new_obj(gui_control_t);
    net_addr_t address = net_ipv4_local_addr;
    address.port = port;
    control->win = win;
    control->keyboard = keyboard;
    control->mouse = mouse;
    control->listener = net_tcp_listen(&address);
    if (!control->listener || !net_sock_set_blocking(control->listener, false)) {
        rvvm_fatal("Unable to listen on control port %u", (unsigned)port);
    }
    rvvm_info("Guest control listening on 127.0.0.1:%u", (unsigned)port);
    return control;
}

void gui_control_poll(gui_control_t* control)
{
    if (!control) {
        return;
    }
    uint64_t now = rvtimer_clocksource(1000);
    if (!control->client) {
        control->client = net_tcp_accept(control->listener);
        if (!control->client) {
            return;
        }
        if (!net_sock_set_blocking(control->client, false)) {
            gui_control_disconnect(control);
            return;
        }
        control->activity = now;
    }
    if (now - control->activity > 10000) {
        gui_control_disconnect(control);
        return;
    }
    if (control->response) {
        size_t amount = EVAL_MIN(control->length - control->sent, 262144);
        int32_t count = net_tcp_send(control->client, control->response + control->sent, amount);
        if (count > 0) {
            control->sent += count;
            control->activity = now;
            if (control->sent == control->length) {
                safe_free(control->response);
                control->length = control->sent = 0;
                if (control->closing) {
                    control->closing = false;
                    gui_backend_on_close(control->win);
                }
            }
        } else if (count != NET_ERR_BLOCK) {
            gui_control_disconnect(control);
        }
        return;
    }
    char* newline = memchr(control->request, '\n', control->received);
    if (!newline) {
        size_t available = sizeof(control->request) - 1 - control->received;
        if (!available) {
            gui_control_disconnect(control);
            return;
        }
        int32_t count = net_tcp_recv(control->client, control->request + control->received, available);
        if (count > 0) {
            control->received += count;
            control->activity = now;
        } else if (count != NET_ERR_BLOCK) {
            gui_control_disconnect(control);
            return;
        }
        newline = memchr(control->request, '\n', control->received);
    }
    if (newline) {
        size_t consumed = newline - control->request + 1;
        *newline = 0;
        gui_control_command(control);
        control->received -= consumed;
        memmove(control->request, control->request + consumed, control->received);
    }
}

void gui_control_free(gui_control_t* control)
{
    if (control) {
        gui_control_disconnect(control);
        net_sock_close(control->listener);
        safe_free(control);
    }
}

#else

gui_control_t* gui_control_init(gui_window_t* win, hid_keyboard_t* keyboard, hid_mouse_t* mouse)
{
    UNUSED(win && keyboard && mouse);
    if (rvvm_has_arg("control")) {
        rvvm_fatal("Guest control requires USE_GUI and USE_NET");
    }
    return NULL;
}

void gui_control_poll(gui_control_t* control) { UNUSED(control); }
void gui_control_free(gui_control_t* control) { UNUSED(control); }

#endif

#ifndef RVVM_GUI_CONTROL_H
#define RVVM_GUI_CONTROL_H

#include "gui_window.h"

typedef struct gui_control gui_control_t;

gui_control_t* gui_control_init(gui_window_t* win, hid_keyboard_t* keyboard, hid_mouse_t* mouse);
void gui_control_poll(gui_control_t* control);
void gui_control_free(gui_control_t* control);

#endif

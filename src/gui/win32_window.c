/*
win32_window.c - Win32 GUI Window
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "feature_test.h"

// Use ANSI for Win9x compat, widechar otherwise
#undef UNICODE
#if !defined(HOST_TARGET_WIN9X)
#define UNICODE
#endif

#include "compiler.h"
#include "gui_window.h"

PUSH_OPTIMIZATION_SIZE

#if defined(HOST_TARGET_WIN32) || defined(HOST_TARGET_CYGWIN)

#include "utils.h"
#include "vma_ops.h"

#include <windows.h>

#define WINDOW_CLASS_NAME  TEXT("LEKKIT_GUI_WINDOW")

#define WINDOW_STYLE_FLAGS (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE)

typedef struct {
    // Window handle
    HWND hwnd;

    // Device context handle
    HDC hdc;

    // Last scanout context
    rvvm_fb_t fb;

    // Input grab flag
    bool grab;
} win32_window_t;

static ATOM win32_atom = 0;

static const hid_key_t win32_key_to_hid_byte_map[] = {
    [0x41] = HID_KEY_A,
    [0x42] = HID_KEY_B,
    [0x43] = HID_KEY_C,
    [0x44] = HID_KEY_D,
    [0x45] = HID_KEY_E,
    [0x46] = HID_KEY_F,
    [0x47] = HID_KEY_G,
    [0x48] = HID_KEY_H,
    [0x49] = HID_KEY_I,
    [0x4A] = HID_KEY_J,
    [0x4B] = HID_KEY_K,
    [0x4C] = HID_KEY_L,
    [0x4D] = HID_KEY_M,
    [0x4E] = HID_KEY_N,
    [0x4F] = HID_KEY_O,
    [0x50] = HID_KEY_P,
    [0x51] = HID_KEY_Q,
    [0x52] = HID_KEY_R,
    [0x53] = HID_KEY_S,
    [0x54] = HID_KEY_T,
    [0x55] = HID_KEY_U,
    [0x56] = HID_KEY_V,
    [0x57] = HID_KEY_W,
    [0x58] = HID_KEY_X,
    [0x59] = HID_KEY_Y,
    [0x5A] = HID_KEY_Z,
    [0x30] = HID_KEY_0,
    [0x31] = HID_KEY_1,
    [0x32] = HID_KEY_2,
    [0x33] = HID_KEY_3,
    [0x34] = HID_KEY_4,
    [0x35] = HID_KEY_5,
    [0x36] = HID_KEY_6,
    [0x37] = HID_KEY_7,
    [0x38] = HID_KEY_8,
    [0x39] = HID_KEY_9,
    [0x0D] = HID_KEY_ENTER,
    [0x1B] = HID_KEY_ESC,
    [0x08] = HID_KEY_BACKSPACE,
    [0x09] = HID_KEY_TAB,
    [0x20] = HID_KEY_SPACE,
    [0xBD] = HID_KEY_MINUS,
    [0xBB] = HID_KEY_EQUAL,
    [0xDB] = HID_KEY_LEFTBRACE,
    [0xDD] = HID_KEY_RIGHTBRACE,
    [0xDC] = HID_KEY_BACKSLASH,
    [0xBA] = HID_KEY_SEMICOLON,
    [0xDE] = HID_KEY_APOSTROPHE,
    [0xC0] = HID_KEY_GRAVE,
    [0xBC] = HID_KEY_COMMA,
    [0xBE] = HID_KEY_DOT,
    [0xBF] = HID_KEY_SLASH,
    [0x14] = HID_KEY_CAPSLOCK,
    [0x70] = HID_KEY_F1,
    [0x71] = HID_KEY_F2,
    [0x72] = HID_KEY_F3,
    [0x73] = HID_KEY_F4,
    [0x74] = HID_KEY_F5,
    [0x75] = HID_KEY_F6,
    [0x76] = HID_KEY_F7,
    [0x77] = HID_KEY_F8,
    [0x78] = HID_KEY_F9,
    [0x79] = HID_KEY_F10,
    [0x7A] = HID_KEY_F11,
    [0x7B] = HID_KEY_F12,
    [0x2C] = HID_KEY_SYSRQ,
    [0x91] = HID_KEY_SCROLLLOCK,
    [0x13] = HID_KEY_PAUSE,
    [0x2D] = HID_KEY_INSERT,
    [0x24] = HID_KEY_HOME,
    [0x21] = HID_KEY_PAGEUP,
    [0x2E] = HID_KEY_DELETE,
    [0x23] = HID_KEY_END,
    [0x22] = HID_KEY_PAGEDOWN,
    [0x27] = HID_KEY_RIGHT,
    [0x25] = HID_KEY_LEFT,
    [0x28] = HID_KEY_DOWN,
    [0x26] = HID_KEY_UP,
    [0x90] = HID_KEY_NUMLOCK,
    [0x6F] = HID_KEY_KPSLASH,
    [0x6A] = HID_KEY_KPASTERISK,
    [0x6D] = HID_KEY_KPMINUS,
    [0x6B] = HID_KEY_KPPLUS,
    [0x6C] = HID_KEY_KPENTER,
    [0x61] = HID_KEY_KP1,
    [0x62] = HID_KEY_KP2,
    [0x63] = HID_KEY_KP3,
    [0x64] = HID_KEY_KP4,
    [0x65] = HID_KEY_KP5,
    [0x66] = HID_KEY_KP6,
    [0x67] = HID_KEY_KP7,
    [0x68] = HID_KEY_KP8,
    [0x69] = HID_KEY_KP9,
    [0x60] = HID_KEY_KP0,
    [0x6E] = HID_KEY_KPDOT,
    [0x5D] = HID_KEY_MENU,
    [0xE2] = HID_KEY_RO, // It's HID_KEY_102ND on Nordic keyboards (I have one),
                         // but Windows has no way to distinguish their VK keycodes
    [0xF2] = HID_KEY_KATAKANAHIRAGANA,
    [0x1C] = HID_KEY_HENKAN,
    [0x1D] = HID_KEY_MUHENKAN,
    [0x15] = HID_KEY_HANGEUL, // Actually KANA on Japanese NEC PC-9800
    [0x19] = HID_KEY_HANJA,
    [0x11] = HID_KEY_LEFTCTRL,
    [0x10] = HID_KEY_LEFTSHIFT,
    [0x12] = HID_KEY_LEFTALT,
    [0x5B] = HID_KEY_LEFTMETA,
    [0xA3] = HID_KEY_RIGHTCTRL,
    [0xA1] = HID_KEY_RIGHTSHIFT,
    [0xA5] = HID_KEY_RIGHTALT,
    [0x5C] = HID_KEY_RIGHTMETA,
};

static hid_key_t win32_key_to_hid(uint32_t win32_key, LPARAM info)
{
    bool extended = !!(info & (1UL << 24));
    if (win32_key == VK_CONTROL) {
        return extended ? HID_KEY_RIGHTCTRL : HID_KEY_LEFTCTRL;
    }
    if (win32_key == VK_MENU) {
        return extended ? HID_KEY_RIGHTALT : HID_KEY_LEFTALT;
    }
    if (win32_key == VK_SHIFT) {
        return ((info >> 16) & 255) == 0x36 ? HID_KEY_RIGHTSHIFT : HID_KEY_LEFTSHIFT;
    }
    if (win32_key == VK_RETURN && extended) {
        return HID_KEY_KPENTER;
    }
    if (win32_key < sizeof(win32_key_to_hid_byte_map)) {
        return win32_key_to_hid_byte_map[win32_key];
    }
    rvvm_warn("Unmapped Win32 keycode %x", win32_key);
    return HID_KEY_NONE;
}

static LRESULT CALLBACK win32_wndproc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
        case WM_CLOSE:
            PostMessage(hwnd, WM_QUIT, wParam, lParam);
            return 0;
        case WM_KILLFOCUS:
            // This is an ugly fucking way to handle WM_KILLFOCUS via PeekMessage()
            PostMessage(hwnd, WM_KILLFOCUS, wParam, lParam);
            return 0;
    }

    if (uMsg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        win32_window_t* state = (win32_window_t*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        SetCursor(state && state->grab ? NULL : LoadCursor(NULL, IDC_ARROW));
        return TRUE;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Clip cursor to window client area
static void win32_clip_cursor(win32_window_t* win32, bool clip)
{
    if (clip) {
        RECT rect = ZERO_INIT;
        GetClientRect(win32->hwnd, &rect);
        MapWindowPoints(win32->hwnd, NULL, (POINT*)&rect, 2);
        ClipCursor(&rect);
    } else {
        ClipCursor(NULL);
    }
}

static void win32_window_free(gui_window_t* win)
{
    win32_window_t* win32 = gui_backend_get_data(win);
    gui_backend_set_data(win, NULL);
    if (win32) {
        win32_clip_cursor(win32, false);
        if (GetCapture() == win32->hwnd) {
            ReleaseCapture();
        }
        vma_free(gui_backend_get_vram(win), gui_backend_get_vram_size(win));
        if (win32->hdc) {
            ReleaseDC(win32->hwnd, win32->hdc);
        }
        if (win32->hwnd) {
            DestroyWindow(win32->hwnd);
        }
        safe_free(win32);
    }
}

static bool win32_absolute_input(void)
{
    return GetSystemMetrics(SM_REMOTESESSION) != 0 || rvvm_has_arg("absolute-mouse") || rvvm_has_arg("usb-input");
}

static void win32_window_poll(gui_window_t* win)
{
    win32_window_t* win32 = gui_backend_get_data(win);

    bool absolute = win32_absolute_input();
    if (win->absolute_mouse != absolute) {
        // RDP reconnect/session migration: release keys and stop cursor warping.
        win->absolute_mouse = absolute;
        gui_backend_on_focus_lost(win);
        rvvm_info("Window mouse mode: %s", absolute ? "absolute (RDP/tablet)" : "relative capture");
    }

    MSG Msg = ZERO_INIT;
    while (PeekMessage(&Msg, win32->hwnd, 0, 0, PM_REMOVE)) {
        switch (Msg.message) {
            case WM_MOUSEMOVE:
                if (win32->grab && !win->absolute_mouse) {
                    POINTS cur = MAKEPOINTS(Msg.lParam);
                    POINT  mid = {
                        .x = gui_window_width(win) >> 1,
                        .y = gui_window_height(win) >> 1,
                    };
                    int dx = cur.x - mid.x;
                    int dy = cur.y - mid.y;
                    if (dx || dy) {
                        ClientToScreen(win32->hwnd, &mid);
                        SetCursorPos(mid.x, mid.y);
                        gui_backend_on_mouse_move(win, dx - (dx / 3), dy - (dy / 3));
                    }
                } else {
                    POINTS cur = MAKEPOINTS(Msg.lParam);
                    gui_backend_on_mouse_place(win, cur.x, cur.y);
                }
                break;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: // For handling F10
                // Disable autorepeat keypresses
                if (!(Msg.lParam & (1UL << 30))) {
                    gui_backend_on_key_press(win, win32_key_to_hid(Msg.wParam, Msg.lParam));
                }
                break;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                gui_backend_on_key_release(win, win32_key_to_hid(Msg.wParam, Msg.lParam));
                break;
            case WM_LBUTTONDOWN:
                gui_backend_on_mouse_press(win, HID_BTN_LEFT);
                break;
            case WM_LBUTTONUP:
                gui_backend_on_mouse_release(win, HID_BTN_LEFT);
                break;
            case WM_RBUTTONDOWN:
                gui_backend_on_mouse_press(win, HID_BTN_RIGHT);
                break;
            case WM_RBUTTONUP:
                gui_backend_on_mouse_release(win, HID_BTN_RIGHT);
                break;
            case WM_MBUTTONDOWN:
                gui_backend_on_mouse_press(win, HID_BTN_MIDDLE);
                break;
            case WM_MBUTTONUP:
                gui_backend_on_mouse_release(win, HID_BTN_MIDDLE);
                break;
            case WM_MOUSEWHEEL:
                gui_backend_on_mouse_scroll(win, -GET_WHEEL_DELTA_WPARAM(Msg.wParam) / 120);
                break;
            case WM_QUIT:
                gui_backend_on_close(win);
                break;
            case WM_KILLFOCUS:
                gui_backend_on_focus_lost(win);
                break;
            default:
                DispatchMessage(&Msg);
                break;
        }
    }
}

// NOTE: Must be called with reserved space in bmi->bmiColors
// NOTE: Windows seems to not accept BI_BITFIELDS for 24bpp, which makes BGR888 support a no-go
static bool win32_handle_formats(BITMAPINFO* bmi, const rvvm_fb_t* fb)
{
    DWORD* mask = (DWORD*)(void*)&bmi->bmiColors;
    switch (rvvm_fb_format(fb)) {
        case RVVM_RGB_XRGB1555:
            mask[0] = 0x7C00;
            mask[1] = 0x03E0;
            mask[2] = 0x001F;
            return true;
        case RVVM_RGB_RGB565:
            mask[0] = 0xF800;
            mask[1] = 0x07E0;
            mask[2] = 0x001F;
            return true;
        case RVVM_RGB_XBGR8888:
            mask[0] = 0x000000FFU;
            mask[1] = 0x0000FF00U;
            mask[2] = 0x00FF0000U;
            return true;
        case RVVM_RGB_BGRX8888:
            mask[0] = 0x0000FF00U;
            mask[1] = 0x00FF0000U;
            mask[2] = 0xFF000000U;
            return true;
        case RVVM_RGB_RGBX8888:
            mask[0] = 0xFF000000U;
            mask[1] = 0x00FF0000U;
            mask[2] = 0x0000FF00U;
            return true;
        case RVVM_RGB_XRGB2101010:
            mask[0] = 0x3FF00000U;
            mask[1] = 0x000FFC00U;
            mask[2] = 0x000003FFU;
            return true;
        case RVVM_RGB_XBGR2101010:
            mask[0] = 0x000003FFU;
            mask[1] = 0x000FFC00U;
            mask[2] = 0x3FF00000U;
            return true;
    }
    return false;
}

static void win32_window_draw(gui_window_t* win, const rvvm_fb_t* fb, uint32_t x, uint32_t y)
{
    win32_window_t* win32 = gui_backend_get_data(win);
    if (rvvm_fb_buffer(fb)) {
        // Bitmap info structure
        BITMAPINFO* bmi = (BITMAPINFO*)safe_calloc(1, sizeof(BITMAPINFO) + (sizeof(DWORD) * 3));

        // Initialize bitmap info
        bmi->bmiHeader.biSize     = sizeof(BITMAPINFOHEADER);
        bmi->bmiHeader.biWidth    = rvvm_fb_stride(fb) / rvvm_fb_rgb_bytes(fb);
        bmi->bmiHeader.biHeight   = -rvvm_fb_height(fb);
        bmi->bmiHeader.biPlanes   = 1;
        bmi->bmiHeader.biBitCount = rvvm_fb_rgb_bpp(fb);

        // Handle special framebuffer formats
        if (win32_handle_formats(bmi, fb)) {
            bmi->bmiHeader.biCompression = BI_BITFIELDS;
        }

        // Draw framebuffer
        SetDIBitsToDevice(win32->hdc, x, y, rvvm_fb_width(fb), rvvm_fb_height(fb), 0, 0, 0, //
                          rvvm_fb_height(fb), rvvm_fb_buffer(fb), bmi, DIB_RGB_COLORS);
        safe_free(bmi);
        win32->fb = *fb;
    }
}

static void win32_window_set_title(gui_window_t* win, const char* title)
{
    win32_window_t* win32 = gui_backend_get_data(win);
#if defined(HOST_TARGET_WIN9X)
    SetWindowTextA(win32->hwnd, title);
#else
    void* title_u16 = utf8_to_utf16(title);
    SetWindowTextW(win32->hwnd, title_u16);
    free(title_u16);
#endif
}

static void win32_window_grab_input(gui_window_t* win, bool grab)
{
    win32_window_t* win32 = gui_backend_get_data(win);
    if (win32->grab != grab) {
        win32->grab = grab;
        SetWindowLongPtr(win32->hwnd, GWLP_USERDATA, (LONG_PTR)win32);
        if (grab) {
            SetCapture(win32->hwnd);
            if (!win->absolute_mouse) {
                POINT mid = {.x = gui_window_width(win) / 2, .y = gui_window_height(win) / 2};
                ClientToScreen(win32->hwnd, &mid);
                SetCursorPos(mid.x, mid.y);
            }
        } else if (GetCapture() == win32->hwnd) {
            ReleaseCapture();
        }
        win32_clip_cursor(win32, grab);
        SetCursor(grab ? NULL : LoadCursor(NULL, IDC_ARROW));
    }
}

static void win32_window_set_win_size(gui_window_t* win, uint32_t w, uint32_t h)
{
    win32_window_t* win32 = gui_backend_get_data(win);
    if (win32) {
        RECT rect = {
            .right  = w,
            .bottom = h,
        };
        uint32_t flags = SWP_DRAWFRAME | SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW;
        AdjustWindowRectEx(&rect, WINDOW_STYLE_FLAGS, false, 0);
        SetWindowPos(win32->hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top, flags);
        if (win32->grab) {
            // Update clip region
            win32_clip_cursor(win32, true);
        }
    }
}

static const gui_backend_cb_t win32_window_cb = {
    .free         = win32_window_free,
    .poll         = win32_window_poll,
    .draw         = win32_window_draw,
    .set_title    = win32_window_set_title,
    .grab_input   = win32_window_grab_input,
    .set_win_size = win32_window_set_win_size,
};

bool win32_window_init(gui_window_t* win)
{
    // Get window size
    RECT rect = {
        .right  = gui_window_width(win),
        .bottom = gui_window_height(win),
    };
    // Register Win32 backend
    win32_window_t* win32 = safe_new_obj(win32_window_t);

    win->absolute_mouse = win32_absolute_input();
    rvvm_info("Window mouse mode: %s", win->absolute_mouse ? "absolute (RDP/tablet)" : "relative capture");
    gui_backend_register(win, &win32_window_cb);
    gui_backend_set_data(win, win32);

    // Create window class atom
    DO_ONCE_SCOPED {
        // Initialize window atom
        WNDCLASS wc = {
            .lpfnWndProc   = win32_wndproc,
            .hInstance     = GetModuleHandle(NULL),
            .lpszClassName = WINDOW_CLASS_NAME,
        };
        win32_atom = RegisterClass(&wc);
    };
    if (!win32_atom) {
        rvvm_error("Failed to register window class");
        return false;
    }

    // Allocate VRAM (Probably better done in GUI middleware later)
    size_t vram_size = gui_backend_get_vram_size(win);
    void*  vram      = vma_alloc(NULL, vram_size, VMA_RDWR);
    if (vram) {
        gui_backend_set_vram(win, vram, vram_size);
    } else {
        rvvm_error("vma_alloc() failed!");
        return false;
    }

    // Adjust window rectangle
    AdjustWindowRectEx(&rect, WINDOW_STYLE_FLAGS, false, 0);

    // Create window
    win32->hwnd = CreateWindow(WINDOW_CLASS_NAME, TEXT(""), WINDOW_STYLE_FLAGS, //
                               CW_USEDEFAULT, CW_USEDEFAULT,                    //
                               rect.right - rect.left, rect.bottom - rect.top,  //
                               NULL, NULL, GetModuleHandle(NULL), NULL);

    // Obtain device context
    if (win32->hwnd) {
        win32->hdc = GetDC(win32->hwnd);
    }

    if (!win32->hwnd || !win32->hdc) {
        rvvm_error("Failed to create window!");
        return false;
    }

    return true;
}

#else

bool win32_window_init(gui_window_t* win)
{
    UNUSED(win);
    return false;
}

#endif

POP_OPTIMIZATION_SIZE

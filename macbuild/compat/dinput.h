// macOS compat: dinput.h (DirectInput)
//
// The client's real input path is being reimplemented on NSEvent; the only
// thing the ported UI code still references from dinput.h is the DIK_* PC
// keyboard scancode constants (config screens map bindings by scancode).
// No DirectInput COM interfaces are used, so this header provides just the
// scancodes actually referenced. Add more DIK_* as needed.
#ifndef __DINPUT_H_COMPAT__
#define __DINPUT_H_COMPAT__

#define DIK_ESCAPE   0x01
#define DIK_PAUSE    0xC5

#define DIK_F1       0x3B
#define DIK_F2       0x3C
#define DIK_F3       0x3D
#define DIK_F4       0x3E
#define DIK_F5       0x3F
#define DIK_F6       0x40
#define DIK_F7       0x41
#define DIK_F8       0x42
#define DIK_F9       0x43
#define DIK_F10      0x44
#define DIK_F11      0x57
#define DIK_F12      0x58
#define DIK_F13      0x64
#define DIK_F14      0x65
#define DIK_F15      0x66

#endif // __DINPUT_H_COMPAT__

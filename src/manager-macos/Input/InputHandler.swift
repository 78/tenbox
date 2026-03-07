import AppKit

class InputHandler {
    // Callbacks to IPC layer
    var onKeyEvent: ((UInt16, Bool) -> Void)?          // (evdev_code, pressed)
    var onPointerEvent: ((Int32, Int32, UInt32) -> Void)? // (abs_x, abs_y, buttons_bitmask)
    var onWheelEvent: ((Int32) -> Void)?                // (delta)

    // Button state: bitmask matching runtime expectation (1=left, 2=right, 4=middle)
    private var buttonMask: UInt32 = 0
    private var lastX: Int32 = 0
    private var lastY: Int32 = 0

    func handleKeyDown(_ event: NSEvent) {
        guard let code = macKeyCodeToEvdev(event.keyCode) else { return }
        onKeyEvent?(code, true)
    }

    func handleKeyUp(_ event: NSEvent) {
        guard let code = macKeyCodeToEvdev(event.keyCode) else { return }
        onKeyEvent?(code, false)
    }

    func handleMouseButton(button: Int, pressed: Bool, absX: Int32, absY: Int32) {
        let bit: UInt32
        switch button {
        case 0: bit = 1  // left
        case 1: bit = 2  // right
        case 2: bit = 4  // middle
        default: return
        }
        if pressed {
            buttonMask |= bit
        } else {
            buttonMask &= ~bit
        }
        lastX = absX
        lastY = absY
        onPointerEvent?(absX, absY, buttonMask)
    }

    func handleMouseMoved(absX: Int32, absY: Int32) {
        lastX = absX
        lastY = absY
        onPointerEvent?(absX, absY, buttonMask)
    }

    private var activeModifierKeys: Set<UInt16> = []

    func handleFlagsChanged(_ event: NSEvent) {
        let keyCode = event.keyCode
        guard let evdev = macKeyCodeToEvdev(keyCode) else { return }
        let flags = event.modifierFlags
        let pressed: Bool
        switch keyCode {
        case 0x38, 0x3C: pressed = flags.contains(.shift)
        case 0x3B, 0x3E: pressed = flags.contains(.control)
        case 0x3A, 0x3D: pressed = flags.contains(.option)
        case 0x37, 0x36: pressed = flags.contains(.command)
        case 0x39:       pressed = flags.contains(.capsLock)
        default:         pressed = !activeModifierKeys.contains(keyCode)
        }
        if pressed {
            activeModifierKeys.insert(keyCode)
        } else {
            activeModifierKeys.remove(keyCode)
        }
        onKeyEvent?(evdev, pressed)
    }

    func releaseAllModifiers() {
        let modifierCodes: [UInt16] = [29, 97, 42, 54, 56, 100, 125, 126]
        for code in modifierCodes {
            onKeyEvent?(code, false)
        }
        if buttonMask != 0 {
            buttonMask = 0
            onPointerEvent?(lastX, lastY, 0)
        }
    }

    private func macKeyCodeToEvdev(_ keyCode: UInt16) -> UInt16? {
        return Self.keyMap[keyCode]
    }

    private static let keyMap: [UInt16: UInt16] = [
        0x12: 2,    // KEY_1
        0x13: 3,    // KEY_2
        0x14: 4,    // KEY_3
        0x15: 5,    // KEY_4
        0x17: 6,    // KEY_5
        0x16: 7,    // KEY_6
        0x1A: 8,    // KEY_7
        0x1C: 9,    // KEY_8
        0x19: 10,   // KEY_9
        0x1D: 11,   // KEY_0
        0x1B: 12,   // KEY_MINUS
        0x18: 13,   // KEY_EQUAL
        0x33: 14,   // KEY_BACKSPACE
        0x30: 15,   // KEY_TAB
        0x0C: 16,   // KEY_Q
        0x0D: 17,   // KEY_W
        0x0E: 18,   // KEY_E
        0x0F: 19,   // KEY_R
        0x11: 20,   // KEY_T
        0x10: 21,   // KEY_Y
        0x20: 22,   // KEY_U
        0x22: 23,   // KEY_I
        0x1F: 24,   // KEY_O
        0x23: 25,   // KEY_P
        0x21: 26,   // KEY_LEFTBRACE
        0x1E: 27,   // KEY_RIGHTBRACE
        0x24: 28,   // KEY_ENTER
        0x00: 30,   // KEY_A
        0x01: 31,   // KEY_S
        0x02: 32,   // KEY_D
        0x03: 33,   // KEY_F
        0x05: 34,   // KEY_G
        0x04: 35,   // KEY_H
        0x26: 36,   // KEY_J
        0x28: 37,   // KEY_K
        0x25: 38,   // KEY_L
        0x29: 39,   // KEY_SEMICOLON
        0x27: 40,   // KEY_APOSTROPHE
        0x32: 41,   // KEY_GRAVE
        0x2A: 43,   // KEY_BACKSLASH
        0x06: 44,   // KEY_Z
        0x07: 45,   // KEY_X
        0x08: 46,   // KEY_C
        0x09: 47,   // KEY_V
        0x0B: 48,   // KEY_B
        0x2D: 49,   // KEY_N
        0x2E: 50,   // KEY_M
        0x2B: 51,   // KEY_COMMA
        0x2F: 52,   // KEY_DOT
        0x2C: 53,   // KEY_SLASH
        0x38: 42,   // KEY_LEFTSHIFT
        0x3C: 54,   // KEY_RIGHTSHIFT
        0x3B: 29,   // KEY_LEFTCTRL
        0x3E: 97,   // KEY_RIGHTCTRL
        0x3A: 56,   // KEY_LEFTALT
        0x3D: 100,  // KEY_RIGHTALT
        0x37: 125,  // KEY_LEFTMETA (Command)
        0x36: 126,  // KEY_RIGHTMETA
        0x31: 57,   // KEY_SPACE
        0x39: 58,   // KEY_CAPSLOCK
        0x35: 1,    // KEY_ESC
        0x7A: 59,   // KEY_F1
        0x78: 60,   // KEY_F2
        0x63: 61,   // KEY_F3
        0x76: 62,   // KEY_F4
        0x60: 63,   // KEY_F5
        0x61: 64,   // KEY_F6
        0x62: 65,   // KEY_F7
        0x64: 66,   // KEY_F8
        0x65: 67,   // KEY_F9
        0x6D: 68,   // KEY_F10
        0x67: 87,   // KEY_F11
        0x6F: 88,   // KEY_F12
        0x7E: 103,  // KEY_UP
        0x7D: 108,  // KEY_DOWN
        0x7B: 105,  // KEY_LEFT
        0x7C: 106,  // KEY_RIGHT
        0x73: 102,  // KEY_HOME
        0x77: 107,  // KEY_END
        0x74: 104,  // KEY_PAGEUP
        0x79: 109,  // KEY_PAGEDOWN
        0x75: 111,  // KEY_DELETE
    ]
}

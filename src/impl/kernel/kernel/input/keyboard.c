#include "input/keyboard.h"
#include "display/print.h"
#include "arch/io.h"
#include "interrupts/idt.h"
#include "devices/serial.h"

static const char scancode_set1[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/',
    0,   '*', 0,  ' ',
    // rest zeroed
};

static void kbd_handler(void) {
    uint8_t sc = inb(0x60);
    if (sc & 0x80) return;          // ignore key release
    char c = scancode_set1[sc & 0x7F];
    // if (c)  {
    //     serial_write_char(c);
    //     print_write_char(c);
    // }
}

void keyboard_init(void) {
    irq_install(1, kbd_handler);
}

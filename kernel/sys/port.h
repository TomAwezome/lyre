#ifndef _SYS__PORT_H
#define _SYS__PORT_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
    asm volatile (
        "outb %0, %1\n\t"
        :
        : "a" (value), "Nd" (port)
        : "memory"
    );
}

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile (
        "outw %0, %1\n\t"
        :
        : "a" (value), "Nd" (port)
        : "memory"
    );
}

static inline void outd(uint16_t port, uint32_t value) {
    asm volatile (
        "outd %0, %1\n\t"
        :
        : "a" (value), "Nd" (port)
        : "memory"
    );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile (
        "inb %1, %0\n\t"
        : "=a" (value)
        : "Nd" (port)
        : "memory"
    );
    return value;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    asm volatile (
        "inw %1, %0\n\t"
        : "=a" (value)
        : "Nd" (port)
        : "memory"
    );
    return value;
}

static inline uint32_t ind(uint16_t port) {
    uint32_t value;
    asm volatile (
        "ind %1, %0\n\t"
        : "=a" (value)
        : "Nd" (port)
        : "memory"
    );
    return value;
}

#endif

#include <stdint.h>
#include <stdarg.h>
#include "unistd.h"
#include "string.h"
#include "stdlib.h"

int putchar(int c) {
    char ch = (char)c;
    sys_write(&ch, 1);
    return c;
}

int puts(const char *s) {
    if (!s) return sys_write("(null)\n", 7);
    sys_write(s, strlen(s));
    sys_write("\n", 1);
    return 0;
}

static void print_dec(int64_t val) {
    char buf[24];
    int neg = 0;
    int pos = 0;
    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) { buf[pos++] = '0'; }
    while (val > 0) { buf[pos++] = '0' + (val % 10); val /= 10; }
    if (neg) buf[pos++] = '-';
    for (int i = pos - 1; i >= 0; i--) putchar(buf[i]);
}

static void print_hex(uint64_t val, int upper) {
    const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[20];
    int pos = 0;
    buf[pos++] = '0'; buf[pos++] = 'x';
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int d = (val >> i) & 0xf;
        if (d || started || i == 0) { started = 1; buf[pos++] = hex[d]; }
    }
    sys_write(buf, pos);
}

int printf(const char *fmt, ...) {
    if (!fmt) return 0;
    va_list ap;
    va_start(ap, fmt);
    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') { putchar(fmt[i]); continue; }
        i++;
        int llong = 0;
        if (fmt[i] == 'l') { llong = 1; i++; }
        switch (fmt[i]) {
        case 'd': case 'i':
            print_dec(llong ? va_arg(ap, int64_t) : va_arg(ap, int));
            break;
        case 'u':
            print_dec(llong ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int));
            break;
        case 'x': case 'X':
            print_hex(llong ? va_arg(ap, uint64_t) : (unsigned)va_arg(ap, int), fmt[i] == 'X');
            break;
        case 'p':
            print_hex((uint64_t)va_arg(ap, void*), 0);
            break;
        case 's': {
            const char *s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            sys_write(s, strlen(s));
            break;
        }
        case 'c':
            putchar(va_arg(ap, int));
            break;
        case '%': putchar('%'); break;
        default: putchar('%'); putchar(fmt[i]); break;
        }
    }
    va_end(ap);
    return 0;
}

static int write_int(char *buf, size_t size, size_t *pos, long long val, int base, int upper) {
    char tmp[32];
    int tpos = 0, neg = 0;
    unsigned long long uval;
    if (base == 10 && val < 0) { neg = 1; uval = -val; }
    else uval = (unsigned long long)val;
    if (uval == 0) tmp[tpos++] = '0';
    while (uval) {
        int d = uval % base;
        tmp[tpos++] = (d < 10) ? '0' + d : (upper ? 'A' : 'a') + (d - 10);
        uval /= base;
    }
    if (neg && *pos < size - 1) buf[(*pos)++] = '-';
    for (int j = tpos - 1; j >= 0 && *pos < size - 1; j--) buf[(*pos)++] = tmp[j];
    return neg ? tpos + 1 : tpos;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    if (!buf || size == 0) return 0;
    size_t pos = 0;
    for (size_t i = 0; fmt[i] && pos < size - 1; i++) {
        if (fmt[i] != '%') { buf[pos++] = fmt[i]; continue; }
        i++;
        int llong = 0;
        if (fmt[i] == 'l') { llong = 1; i++; }
        if (fmt[i] == 'l') { llong = 2; i++; }
        switch (fmt[i]) {
        case 'd': case 'i':
            if (llong >= 2) write_int(buf, size, &pos, va_arg(ap, long long), 10, 0);
            else if (llong) write_int(buf, size, &pos, va_arg(ap, long), 10, 0);
            else write_int(buf, size, &pos, va_arg(ap, int), 10, 0);
            break;
        case 'u':
            if (llong >= 2) write_int(buf, size, &pos, va_arg(ap, unsigned long long), 10, 0);
            else if (llong) write_int(buf, size, &pos, va_arg(ap, unsigned long), 10, 0);
            else write_int(buf, size, &pos, va_arg(ap, unsigned int), 10, 0);
            break;
        case 'x': case 'X':
            if (llong >= 2) write_int(buf, size, &pos, va_arg(ap, unsigned long long), 16, fmt[i] == 'X');
            else if (llong) write_int(buf, size, &pos, va_arg(ap, unsigned long), 16, fmt[i] == 'X');
            else write_int(buf, size, &pos, va_arg(ap, unsigned int), 16, fmt[i] == 'X');
            break;
        case 'p':
            write_int(buf, size, &pos, (long long)va_arg(ap, void*), 16, 0);
            break;
        case 's': {
            const char *s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            while (*s && pos < size - 1) buf[pos++] = *s++;
            break;
        }
        case 'c':
            if (pos < size - 1) buf[pos++] = va_arg(ap, int);
            break;
        case '%':
            if (pos < size - 1) buf[pos++] = '%';
            break;
        default:
            if (pos < size - 1) buf[pos++] = fmt[i];
            break;
        }
    }
    buf[pos] = 0;
    return pos;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return ret;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}
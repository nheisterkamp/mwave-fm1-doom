/* SPDX-License-Identifier: GPL-3.0-only */
/* What the engine needs from a C library and the JieLi newlib does not have: the printf family (no
 * floating point), malloc on the engine's zone, memmove. printf goes to the USB serial console. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "doomgeneric.h"
#include "z_zone.h"

typedef struct {
    char *buf;
    size_t size, len;            /* len counts every character, written or not */
} out_t;

static void out_c(out_t *o, char c)
{
    if (o->len + 1u < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void out_num(out_t *o, unsigned long long v, unsigned base, int upper, int neg, int width, int zero,
                    int left, int prec)
{
    char tmp[24];
    int n = 0, pad, i;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    while (n < prec && n < (int)sizeof tmp)
        tmp[n++] = '0';
    pad = width - n - (neg ? 1 : 0);
    if (!left && !zero)
        for (; pad > 0; pad--)
            out_c(o, ' ');
    if (neg)
        out_c(o, '-');
    if (!left && zero)
        for (; pad > 0; pad--)
            out_c(o, '0');
    for (i = n - 1; i >= 0; i--)
        out_c(o, tmp[i]);
    if (left)
        for (; pad > 0; pad--)
            out_c(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    out_t o = {buf, size, 0};
    for (; *fmt; fmt++) {
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
        if (*fmt != '%') {
            out_c(&o, *fmt);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-')
                left = 1;
            else if (*fmt == '0')
                zero = 1;
            else if (*fmt != '+' && *fmt != ' ' && *fmt != '#')
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        } else
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
            if (*fmt == 'l')
                lng++;
            fmt++;
        }
        switch (*fmt) {
        case 'd': case 'i': {
            long long v = lng > 1 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
            out_num(&o, v < 0 ? (unsigned long long)-v : (unsigned long long)v, 10, 0, v < 0, width, zero, left,
                    prec);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng > 1 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) :
                                   va_arg(ap, unsigned);
            out_num(&o, v, *fmt == 'o' ? 8u : *fmt == 'u' ? 10u : 16u, *fmt == 'X', 0, width, zero, left, prec);
            break;
        }
        case 'p':
            out_c(&o, '0');
            out_c(&o, 'x');
            out_num(&o, (uintptr_t)va_arg(ap, void *), 16, 0, 0, 8, 1, 0, -1);
            break;
        case 'c':
            out_c(&o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n = 0, pad;
            if (!s)
                s = "(null)";
            while (s[n] && (prec < 0 || n < prec))
                n++;
            pad = width - n;
            if (!left)
                for (; pad > 0; pad--)
                    out_c(&o, ' ');
            while (n--)
                out_c(&o, *s++);
            if (left)
                for (; pad > 0; pad--)
                    out_c(&o, ' ');
            break;
        }
        case '%':
            out_c(&o, '%');
            break;
        case 'f': case 'g': case 'e':
            (void)va_arg(ap, double);  /* no floating point here */
            out_c(&o, '?');
            break;
        default:
            if (!*fmt)
                fmt--;
            break;
        }
    }
    if (size)
        buf[o.len < size ? o.len : size - 1u] = 0;
    return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, 1024, fmt, ap);
    va_end(ap);
    return n;
}

/* printf: lines to the console */
static char line[128];
static size_t line_len;

int putchar(int c)
{
    if (c == '\n' || line_len + 1u >= sizeof line) {
        line[line_len] = 0;
        DG_Log(line);
        line_len = 0;
        if (c == '\n')
            return c;
    }
    line[line_len++] = (char)c;
    return c;
}

int puts(const char *s)
{
    while (*s)
        putchar(*s++);
    putchar('\n');
    return 0;
}

int vprintf(const char *fmt, va_list ap)
{
    char b[256];
    int n = vsnprintf(b, sizeof b, fmt, ap), i;
    for (i = 0; b[i]; i++)
        putchar(b[i]);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* malloc: the zone (the few engine allocations outside Z_Malloc) */
void *malloc(size_t n)
{
    return Z_Malloc((int)n, PU_STATIC, NULL);
}

void *calloc(size_t n, size_t k)
{
    unsigned char *p = malloc(n * k);
    size_t i;
    for (i = 0; i < n * k; i++)
        p[i] = 0;
    return p;
}

void free(void *p)
{
    if (p)
        Z_Free(p);
}

char *strdup(const char *s)
{
    size_t n = 0;
    char *d;
    while (s[n])
        n++;
    d = malloc(n + 1u);
    for (n = 0; (d[n] = s[n]) != 0; n++)
        ;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp < sp)
        while (n--)
            *dp++ = *sp++;
    else
        while (n--)
            dp[n] = sp[n];
    return d;
}

void exit(int code)
{
    (void)code;
    DG_Quit();
}

void abort(void)
{
    DG_Error("abort");
}

/* stdio: no files on the FM-1. Opening fails; writing to a stream goes to the console. */
#include <stdio.h>

FILE *fopen(const char *name, const char *mode)
{
    (void)name;
    (void)mode;
    return NULL;
}

int fclose(FILE *f) { (void)f; return 0; }
size_t fread(void *p, size_t s, size_t n, FILE *f) { (void)p; (void)s; (void)n; (void)f; return 0; }
size_t fwrite(const void *p, size_t s, size_t n, FILE *f) { (void)p; (void)s; (void)f; return n; }
long ftell(FILE *f) { (void)f; return -1; }
int fseek(FILE *f, long o, int w) { (void)f; (void)o; (void)w; return -1; }
int remove(const char *p) { (void)p; return -1; }
int rename(const char *a, const char *b) { (void)a; (void)b; return -1; }

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;
    (void)f;
    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    (void)f;
    return vprintf(fmt, ap);
}

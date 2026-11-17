#ifndef FULMEN_ULIB_H
#define FULMEN_ULIB_H

#include "types.h"
#include "syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

int64_t u_syscall(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2);
void sys_exit(int code) __attribute__((noreturn));
ssize_t sys_write(int fd, const void *buf, size_t len);
ssize_t sys_read(int fd, void *buf, size_t len);
void sys_yield(void);
void sys_sleep(uint64_t ms);
int sys_getpid(void);
int sys_getel(void);
uint64_t sys_uptime(void);
void *sys_sbrk(int64_t inc);

int puts(const char *s);
int putchar(int c);
int printf(const char *fmt, ...);
int atoi(const char *s);
size_t strlen(const char *s);
void *malloc(size_t size);
void free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* FULMEN_ULIB_H */

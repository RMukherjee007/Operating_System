#ifndef FULMEN_TYPES_H
#define FULMEN_TYPES_H

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long      uint64_t;
typedef signed long        int64_t;

typedef uint64_t           uintptr_t;
typedef int64_t            intptr_t;
typedef uint64_t           size_t;
typedef int64_t            ssize_t;

#ifndef __cplusplus
typedef uint8_t            bool;
#define true               1
#define false              0
#endif

#ifndef NULL
#define NULL               ((void *)0)
#endif

#define UNUSED(x)          ((void)(x))
#define ALIGN_UP(x, a)     (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a)   ((x) & ~((uint64_t)(a) - 1))

#define ARRAY_SIZE(arr)    (sizeof(arr) / sizeof((arr)[0]))

/* Standard error codes (negative values returned on error) */
#define EPERM              1
#define ENOENT             2
#define ESRCH              3
#define EIO                5
#define ENOMEM             12
#define EACCES             13
#define EBUSY              16
#define EEXIST             17
#define ENOTDIR            20
#define EISDIR             21
#define EINVAL             22
#define ENOSPC             28
#define EDEADLK            35
#define ENOTEMPTY          39

/* Variadic argument macros using GCC/Clang builtins (no libc needed) */
typedef __builtin_va_list  va_list;
#define va_start(v, l)     __builtin_va_start(v, l)
#define va_end(v)          __builtin_va_end(v)
#define va_arg(v, l)       __builtin_va_arg(v, l)

#endif /* FULMEN_TYPES_H */

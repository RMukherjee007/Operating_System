#include "ulib.h"

void *operator new(size_t size) {
    return malloc(size);
}

void operator delete(void *ptr) noexcept {
    free(ptr);
}

void operator delete(void *ptr, size_t) noexcept {
    free(ptr);
}

class Matrix2x2 {
public:
    int64_t m[2][2];

    Matrix2x2(int64_t a, int64_t b, int64_t c, int64_t d) {
        m[0][0] = a; m[0][1] = b;
        m[1][0] = c; m[1][1] = d;
    }

    Matrix2x2 operator*(const Matrix2x2 &rhs) const {
        return Matrix2x2(
            m[0][0] * rhs.m[0][0] + m[0][1] * rhs.m[1][0],
            m[0][0] * rhs.m[0][1] + m[0][1] * rhs.m[1][1],
            m[1][0] * rhs.m[0][0] + m[1][1] * rhs.m[1][0],
            m[1][0] * rhs.m[0][1] + m[1][1] * rhs.m[1][1]
        );
    }

    int64_t det() const {
        return m[0][0] * m[1][1] - m[0][1] * m[1][0];
    }
};

class Calculator {
public:
    int64_t gcd(int64_t a, int64_t b) {
        while (b != 0) {
            int64_t t = b;
            b = a % b;
            a = t;
        }
        return a;
    }

    int64_t factorial(int64_t n) {
        int64_t r = 1;
        for (int64_t i = 2; i <= n; i++) r *= i;
        return r;
    }
};

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("[EL%d C++ Program] Running compiled C++20 OOP demo in user space!\n", sys_getel());

    Calculator *calc = new Calculator();
    printf("  Calculator::factorial(10) = %ld\n", calc->factorial(10));
    printf("  Calculator::gcd(252, 105) = %ld\n", calc->gcd(252, 105));
    delete calc;

    Matrix2x2 A(1, 2, 3, 4);
    Matrix2x2 B(2, 0, 1, 2);
    Matrix2x2 C = A * B;
    printf("  Matrix2x2 (A * B) = [[%ld, %ld], [%ld, %ld]], det=%ld\n",
           C.m[0][0], C.m[0][1], C.m[1][0], C.m[1][1], C.det());

    return 0;
}

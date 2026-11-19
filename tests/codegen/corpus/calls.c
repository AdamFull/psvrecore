/* Synthetic corpus: call-heavy code in Thumb-2 (small leaf functions, recursion, calls through C++-style virtual
 * tables, callbacks, tail calls, calls between Thumb and ARM functions). See integer.c. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))
#define ARM_CODE __attribute__((target("arm")))

typedef struct Vec {
    int32_t x, y, z;
} Vec;

NOINLINE int32_t get_x(const Vec* v) { return v->x; }
NOINLINE int32_t get_y(const Vec* v) { return v->y; }
NOINLINE int32_t get_z(const Vec* v) { return v->z; }
NOINLINE void set_x(Vec* v, int32_t x) { v->x = x; }
NOINLINE int32_t add3(int32_t a, int32_t b, int32_t c) { return a + b + c; }
NOINLINE int32_t mix(int32_t a, int32_t b) { return (a * 31) ^ (b >> 3); }

/* One call every few instructions: the getters of an engine's vector class. */
/* @test vec_sum r0=ptr:4 r1=int:0:32 */
NOINLINE int32_t vec_sum(Vec* v, uint32_t n) {
    int32_t s = 0;
    for (uint32_t i = 0; i < n; ++i) {
        s = add3(s, get_x(&v[i]), mix(get_y(&v[i]), get_z(&v[i])));
        if (s & 1) set_x(&v[i], s);
    }
    return s;
}

/* @test fib r0=int:0:14 */
NOINLINE uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

/* Recursion with stack arguments (more than four). */
NOINLINE int32_t many_args(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f, int32_t depth) {
    if (depth <= 0) return a ^ b ^ c ^ d ^ e ^ f;
    return many_args(b, c, d, e, f, a + depth, depth - 1) + e - f;
}
/* @test call_many_args r0=any r1=any r2=any r3=int:0:12 */
NOINLINE int32_t call_many_args(int32_t a, int32_t b, int32_t c, int32_t depth) {
    return many_args(a, b, c, a ^ b, b ^ c, c ^ a, depth);
}

/* Virtual calls: objects whose first word is a virtual table in read-only data; the call loads the table and then
 * the function, and calls through a register. */
typedef struct Shape Shape;
typedef struct ShapeVtbl {
    int32_t (*area)(const Shape*);
    int32_t (*scale)(Shape*, int32_t);
} ShapeVtbl;
struct Shape {
    const ShapeVtbl* vt;
    int32_t a, b;
};

NOINLINE int32_t rect_area(const Shape* s) { return s->a * s->b; }
NOINLINE int32_t rect_scale(Shape* s, int32_t k) { s->a *= k; s->b *= k; return s->a; }
NOINLINE int32_t tri_area(const Shape* s) { return (s->a * s->b) >> 1; }
NOINLINE int32_t tri_scale(Shape* s, int32_t k) { s->a += k; return s->b - k; }
ARM_CODE NOINLINE int32_t circle_area(const Shape* s) { return 3 * s->a * s->a + (s->b & 7); }
ARM_CODE NOINLINE int32_t circle_scale(Shape* s, int32_t k) { s->a = s->a * k - 1; return s->a ^ k; }

static const ShapeVtbl rect_vt = {rect_area, rect_scale};
static const ShapeVtbl tri_vt = {tri_area, tri_scale};
static const ShapeVtbl circle_vt = {circle_area, circle_scale};
static const ShapeVtbl* const vtables[4] = {&rect_vt, &tri_vt, &circle_vt, &rect_vt};

/* @test virtual_calls r0=ptr:4 r1=int:0:20 r2=any */
NOINLINE int32_t virtual_calls(Shape* shapes, uint32_t n, uint32_t kinds) {
    for (uint32_t i = 0; i < n; ++i) {
        shapes[i].vt = vtables[(kinds >> (2 * (i & 15))) & 3];
        shapes[i].a &= 0xFFF;
        shapes[i].b &= 0xFFF;
    }
    int32_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        Shape* s = &shapes[i];
        total += s->vt->area(s);
        total ^= s->vt->scale(s, (int32_t)(i & 3) + 1);
    }
    return total;
}

/* Callbacks given as arguments. */
typedef int32_t (*Reduce)(int32_t, int32_t);
NOINLINE int32_t r_add(int32_t a, int32_t b) { return a + b; }
NOINLINE int32_t r_max(int32_t a, int32_t b) { return a > b ? a : b; }
ARM_CODE NOINLINE int32_t r_xor(int32_t a, int32_t b) { return a ^ (b * 3); }
NOINLINE int32_t fold(const int32_t* p, uint32_t n, Reduce f, int32_t init) {
    for (uint32_t i = 0; i < n; ++i) init = f(init, p[i]);
    return init;
}
static const Reduce reducers[3] = {r_add, r_max, r_xor};
/* @test fold_all r0=ptr:4 r1=int:0:40 */
NOINLINE int32_t fold_all(const int32_t* p, uint32_t n) {
    int32_t r = 0;
    for (uint32_t k = 0; k < 3; ++k) r = r * 7 + fold(p, n, reducers[k], (int32_t)k);
    return r;
}

/* Tail calls (b.w to another function). */
NOINLINE int32_t tail_target_a(int32_t x, int32_t y) { return x * y - 1; }
NOINLINE int32_t tail_target_b(int32_t x, int32_t y) { return (x << 2) + y; }
ARM_CODE NOINLINE int32_t tail_target_arm(int32_t x, int32_t y) { return x - (y >> 1); }
/* @test tail_dispatch r0=int:0:3 r1=any r2=any */
NOINLINE int32_t tail_dispatch(uint32_t k, int32_t x, int32_t y) {
    if (k == 0) return tail_target_a(x, y);
    if (k == 1) return tail_target_b(y, x);
    return tail_target_arm(x + 1, y);
}

/* An ARM function calling Thumb ones (blx immediate) and the other way. */
/* @test arm_caller r0=ptr:4 r1=int:0:16 */
ARM_CODE NOINLINE int32_t arm_caller(Vec* v, uint32_t n) {
    int32_t s = 0;
    for (uint32_t i = 0; i < n; ++i) s = mix(s, get_x(&v[i]) + get_z(&v[i]));
    return s;
}
/* @test thumb_calls_arm r0=ptr:4 r1=int:0:16 */
NOINLINE int32_t thumb_calls_arm(Vec* v, uint32_t n) { return arm_caller(v, n) + tail_target_arm(n, 3); }

/* A leaf that keeps state in memory, called in a loop with stack-allocated arguments. */
NOINLINE void accumulate(int32_t* acc, const int32_t* v, uint32_t i) { acc[i & 3] += v[0] - v[1]; }
/* @test stack_args r0=ptr:4 r1=int:0:30 */
NOINLINE int32_t stack_args(const int32_t* p, uint32_t n) {
    int32_t acc[4] = {1, 2, 3, 4};
    for (uint32_t i = 0; i + 1 < n; ++i) {
        int32_t pair[2] = {p[i], p[i + 1] ^ (int32_t)i};
        accumulate(acc, pair, i);
    }
    return acc[0] ^ acc[1] ^ acc[2] ^ acc[3];
}

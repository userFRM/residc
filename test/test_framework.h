#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int _tf_pass = 0, _tf_fail = 0;

#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        printf("  FAIL %s:%d: %s == %lld, expected %s == %lld\n", \
               __FILE__, __LINE__, #a, _a, #b, _b); \
        _tf_fail++; return; \
    } \
} while(0)

#define ASSERT_NEQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a == _b) { \
        printf("  FAIL %s:%d: %s == %lld, expected != %s\n", \
               __FILE__, __LINE__, #a, _a, #b); \
        _tf_fail++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("  FAIL %s:%d: !(%s)\n", __FILE__, __LINE__, #x); \
        _tf_fail++; return; \
    } \
} while(0)

#define ASSERT_GE(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a < _b) { \
        printf("  FAIL %s:%d: %s == %lld, expected >= %s == %lld\n", \
               __FILE__, __LINE__, #a, _a, #b, _b); \
        _tf_fail++; return; \
    } \
} while(0)

#define ASSERT_LE(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a > _b) { \
        printf("  FAIL %s:%d: %s == %lld, expected <= %s == %lld\n", \
               __FILE__, __LINE__, #a, _a, #b, _b); \
        _tf_fail++; return; \
    } \
} while(0)

#define ASSERT_MEM_EQ(a, b, n) do { \
    if (memcmp((a), (b), (n)) != 0) { \
        printf("  FAIL %s:%d: memcmp(%s, %s, %d)\n", \
               __FILE__, __LINE__, #a, #b, (int)(n)); \
        _tf_fail++; return; \
    } \
} while(0)

#define RUN_TEST(fn) do { \
    int _prev = _tf_fail; \
    fn(); \
    if (_tf_fail == _prev) { printf("  ok: %s\n", #fn); _tf_pass++; } \
} while(0)

#define TEST_SUMMARY() do { \
    printf("\n%d passed, %d failed\n", _tf_pass, _tf_fail); \
    return _tf_fail > 0 ? 1 : 0; \
} while(0)

#endif

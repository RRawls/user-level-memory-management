#include "../my_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern unsigned long missCount, checkCount;

static int failures = 0;
#define CHECK(cond, msg) do { if(cond){ printf("PASS: %s\n", msg);} else { printf("FAIL: %s\n", msg); failures++; } } while(0)

int main(void){
    set_physical_mem();

    //basic round trip across a page boundary (3 pages, start mid-page)
    unsigned int p = (unsigned int)(uintptr_t)t_malloc(3 * 8192);
    CHECK(p != 0, "first t_malloc is non-NULL (page 0 reserved)");
    size_t len = 20000;
    char *src = malloc(3 * 8192), *dst = calloc(1, 3 * 8192);
    for(size_t i = 0; i < 3 * 8192; i++) src[i] = (char)(i * 31 + 7);
    CHECK(put_value(p + 100, src, len) == 0, "put_value across pages");
    CHECK(get_value(p + 100, dst, len) == 0 && memcmp(src, dst, len) == 0, "get_value matches put_value");

    //writing past the allocation must fail
    CHECK(put_value(p + 100, src, 3 * 8192) == -1, "put_value past end fails");

    //free invalidates translation, including via the TLB
    CHECK(translate(p) != NULL, "translate works before free (fills TLB)");
    CHECK(t_free(p, 3 * 8192) == 0, "t_free succeeds");
    CHECK(translate(p) == NULL, "translate NULL after free (stale TLB entry gone)");
    CHECK(t_free(p, 8192) == -1, "double free rejected");

    //unmapped translate must not touch the counters
    unsigned long m0 = missCount, c0 = checkCount;
    translate(p);
    CHECK(missCount == m0 && checkCount == c0, "unmapped translate not counted as TLB miss");

    //freed pages can be reused
    unsigned int q = (unsigned int)(uintptr_t)t_malloc(8192);
    CHECK(q == p, "freed virtual page reused");
    t_free(q, 8192);

    //TLB wraparound: touch > TLB_ENTRIES pages, then re-touch recent ones -> hits
    unsigned int big = (unsigned int)(uintptr_t)t_malloc((TLB_ENTRIES + 64) * 8192UL);
    CHECK(big != 0, "large allocation");
    for(int i = 0; i < TLB_ENTRIES + 64; i++) translate(big + i * 8192);
    unsigned long m1 = missCount;
    for(int i = 64; i < TLB_ENTRIES + 64; i++) translate(big + i * 8192); //most recent 256
    CHECK(missCount == m1, "no misses re-touching the 256 most recent pages after wrap");

    //mat_mult correctness
    enum { L = 5, M = 7, N = 6 };
    unsigned int A = (unsigned int)(uintptr_t)t_malloc(L*M*sizeof(int));
    unsigned int B = (unsigned int)(uintptr_t)t_malloc(M*N*sizeof(int));
    unsigned int C = (unsigned int)(uintptr_t)t_malloc(L*N*sizeof(int));
    int a[L*M], b[M*N], c[L*N], want[L*N];
    for(int i = 0; i < L*M; i++) a[i] = i + 1;
    for(int i = 0; i < M*N; i++) b[i] = (i % 5) - 2;
    for(int i = 0; i < L; i++) for(int j = 0; j < N; j++){ int s = 0; for(int k = 0; k < M; k++) s += a[i*M+k]*b[k*N+j]; want[i*N+j] = s; }
    put_value(A, a, sizeof a); put_value(B, b, sizeof b);
    mat_mult(A, B, C, L, M, N);
    get_value(C, c, sizeof c);
    CHECK(memcmp(c, want, sizeof c) == 0, "mat_mult matches reference");

    //regression tests for reserved page 0, range checks, zeroing, atomic puts
    CHECK(translate(0) == NULL, "page 0 is never translatable");
    CHECK(t_free(0, 8192) == -1, "freeing reserved page 0 rejected");
    CHECK(t_free(0xFFFFE000u, 1u << 30) == -1, "free beyond address space rejected");
    CHECK(put_value(0xFFFFF000u, src, 8192) == -1, "put_value crossing 4GB wrap fails");

    unsigned int z = (unsigned int)(uintptr_t)t_malloc(8192);
    char junk[8192]; memset(junk, 0xAB, sizeof junk);
    put_value(z, junk, sizeof junk); t_free(z, 8192);
    unsigned int z2 = (unsigned int)(uintptr_t)t_malloc(8192);
    char chk[8192]; get_value(z2, chk, sizeof chk);
    CHECK(chk[0] == 0 && chk[8191] == 0, "reallocated memory is zeroed");

    //failed put must not modify anything
    char ones[8192]; memset(ones, 0x11, sizeof ones);
    put_value(z2, ones, sizeof ones);
    char big2[2 * 8192]; memset(big2, 0x22, sizeof big2);
    CHECK(put_value(z2, big2, sizeof big2) == -1, "put spanning into unmapped page fails");
    get_value(z2, chk, sizeof chk);
    CHECK(memcmp(chk, ones, sizeof ones) == 0, "failed put_value left memory untouched");
    t_free(z2, 8192);

    //mid-page free spanning two pages
    unsigned int w = (unsigned int)(uintptr_t)t_malloc(2 * 8192);
    CHECK(t_free(w + 8000, 400) == 0, "unaligned free spanning two pages succeeds");
    CHECK(translate(w) == NULL && translate(w + 8192) == NULL, "both pages unmapped after unaligned free");

    print_TLB_missrate();
    free(src); free(dst);
    printf(failures ? "\n%d FAILED\n" : "\nALL PASSED\n", failures);
    return failures != 0;
}

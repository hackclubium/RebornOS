/* Proves the three graphics-phase kernel primitives work from real
 * ring-3 code: mapping and drawing to the framebuffer, reading the
 * mouse, and growing a heap via SYS_SBRK. No libc, no crt0 -- same
 * convention as every other userland program here, and (like all of
 * them) this doesn't share a header with the kernel, so the syscall
 * numbers and struct layouts below must match kernel/include/syscall.h
 * exactly. */
#define SYS_WRITE      1
#define SYS_EXIT       2
#define SYS_FB_INFO    8
#define SYS_FB_MAP     9
#define SYS_MOUSE_READ 10
#define SYS_SBRK       11

static inline long sys_write(const char *s) {
    long ret = SYS_WRITE;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(s) : "memory", "cc");
    return ret;
}

static inline void sys_exit(long code) {
    __asm__ volatile("int $0x80" : : "a"((long)SYS_EXIT), "D"(code));
    __builtin_unreachable();
}

typedef struct {
    unsigned int width;
    unsigned int height;
    unsigned int pixels_per_scanline;
    unsigned int bytes_per_pixel;
    unsigned char red_shift;
    unsigned char green_shift;
    unsigned char blue_shift;
    unsigned char _pad;
} fb_info_t;

static inline long sys_fb_info(fb_info_t *out) {
    long ret = SYS_FB_INFO;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(out) : "memory", "cc");
    return ret;
}

static inline long sys_fb_map(void) {
    long ret = SYS_FB_MAP;
    __asm__ volatile("int $0x80" : "+a"(ret) : : "memory", "cc");
    return ret;
}

typedef struct {
    int x;
    int y;
    unsigned char buttons;
    unsigned char _pad[3];
} mouse_state_t;

static inline long sys_mouse_read(mouse_state_t *out) {
    long ret = SYS_MOUSE_READ;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(out) : "memory", "cc");
    return ret;
}

static inline long sys_sbrk(long increment) {
    long ret = SYS_SBRK;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(increment) : "memory", "cc");
    return ret;
}

#define HEAP_TEST_BYTES (3 * 4096 + 100) /* spans several pages, well past one demand-paged page */

void _start(void) {
    fb_info_t info;
    if (sys_fb_info(&info) != 0) {
        sys_write("gfxtest: SYS_FB_INFO failed\n");
        sys_exit(1);
    }

    long fb_vaddr = sys_fb_map();
    if (fb_vaddr == 0) {
        sys_write("gfxtest: SYS_FB_MAP failed\n");
        sys_exit(1);
    }

    /* 0xAB, 0xCD, 0xEF packed the same way fb_pack_color() does
     * kernel-side -- kmain.c's self-test recomputes the same value and
     * compares it against what actually landed in framebuffer memory. */
    unsigned int color = ((unsigned int)0xABu << info.red_shift) |
                          ((unsigned int)0xCDu << info.green_shift) |
                          ((unsigned int)0xEFu << info.blue_shift);
    volatile unsigned int *fb = (volatile unsigned int *)(unsigned long)fb_vaddr;
    unsigned int stride = info.pixels_per_scanline; /* in pixels, not bytes -- fb is uint32_t* */
    fb[10 * stride + 10] = color;

    mouse_state_t mstate;
    if (sys_mouse_read(&mstate) != 0) {
        sys_write("gfxtest: SYS_MOUSE_READ failed\n");
        sys_exit(1);
    }

    long heap_start = sys_sbrk(0);
    long returned = sys_sbrk(HEAP_TEST_BYTES);
    if (returned != heap_start) {
        sys_write("gfxtest: SYS_SBRK failed\n");
        sys_exit(1);
    }

    volatile unsigned char *heap = (volatile unsigned char *)(unsigned long)heap_start;
    for (long i = 0; i < HEAP_TEST_BYTES; i++) {
        heap[i] = (unsigned char)(i & 0xFF);
    }
    for (long i = 0; i < HEAP_TEST_BYTES; i++) {
        if (heap[i] != (unsigned char)(i & 0xFF)) {
            sys_write("gfxtest: heap verification failed\n");
            sys_exit(1);
        }
    }

    sys_write("gfxtest: ok\n");
    sys_exit(0);
}

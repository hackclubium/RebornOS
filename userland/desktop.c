/* First graphical userspace: maps the kernel-provided framebuffer and
 * owns the screen directly. No windowing, font, or compositor yet --
 * just enough to make graphical mode the normal boot path. */
#define SYS_EXIT       2
#define SYS_FB_INFO    8
#define SYS_FB_MAP     9
#define SYS_MOUSE_READ 10

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

typedef struct {
    int x;
    int y;
    unsigned char buttons;
    unsigned char _pad[3];
} mouse_state_t;

static inline void sys_exit(long code) {
    __asm__ volatile("int $0x80" : : "a"((long)SYS_EXIT), "D"(code));
    __builtin_unreachable();
}

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

static inline long sys_mouse_read(mouse_state_t *out) {
    long ret = SYS_MOUSE_READ;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(out) : "memory", "cc");
    return ret;
}

static fb_info_t fb_info;
static volatile unsigned int *fb;

static unsigned int rgb(unsigned int color) {
    unsigned int r = (color >> 16) & 0xFF;
    unsigned int g = (color >> 8) & 0xFF;
    unsigned int b = color & 0xFF;
    return (r << fb_info.red_shift) | (g << fb_info.green_shift) | (b << fb_info.blue_shift);
}

static void pixel(int x, int y, unsigned int color) {
    if (x < 0 || y < 0 || x >= (int)fb_info.width || y >= (int)fb_info.height) {
        return;
    }
    fb[(unsigned int)y * fb_info.pixels_per_scanline + (unsigned int)x] = color;
}

static void rect(int x, int y, int w, int h, unsigned int color) {
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            pixel(x + xx, y + yy, color);
        }
    }
}

static void draw_cursor(int x, int y, unsigned int body, unsigned int edge) {
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col <= row / 2; col++) {
            pixel(x + col, y + row, col == 0 || col == row / 2 ? edge : body);
        }
    }
    rect(x + 4, y + 11, 3, 7, edge);
}

static void draw_desktop(void) {
    unsigned int bg0 = rgb(0x101827);
    unsigned int bg1 = rgb(0x14233a);
    unsigned int panel = rgb(0x0b1020);
    unsigned int card = rgb(0x24344f);
    unsigned int accent = rgb(0x4cc9f0);
    for (unsigned int y = 0; y < fb_info.height; y++) {
        unsigned int color = y < fb_info.height / 2 ? bg0 : bg1;
        for (unsigned int x = 0; x < fb_info.width; x++) {
            fb[y * fb_info.pixels_per_scanline + x] = color;
        }
    }
    rect(0, 0, (int)fb_info.width, 34, panel);
    rect(18, 10, 96, 14, accent);
    rect(40, 76, 180, 120, card);
    rect(66, 104, 128, 12, accent);
    rect(66, 130, 92, 10, rgb(0x8ecae6));
    rect((int)fb_info.width - 230, 76, 180, 120, card);
    rect((int)fb_info.width - 204, 104, 128, 12, rgb(0xffb703));
    rect((int)fb_info.width - 204, 130, 92, 10, rgb(0xf4a261));
}

void _start(void) {
    if (sys_fb_info(&fb_info) != 0) {
        sys_exit(1);
    }
    long fb_vaddr = sys_fb_map();
    if (fb_vaddr == 0 || fb_info.bytes_per_pixel != 4) {
        sys_exit(1);
    }
    fb = (volatile unsigned int *)(unsigned long)fb_vaddr;
    draw_desktop();

    int old_x = -1;
    int old_y = -1;
    mouse_state_t mouse;
    for (;;) {
        if (sys_mouse_read(&mouse) == 0) {
            if (mouse.x != old_x || mouse.y != old_y) {
                draw_desktop();
                draw_cursor(mouse.x, mouse.y, rgb(0xffffff), rgb(0x0b1020));
                old_x = mouse.x;
                old_y = mouse.y;
            }
        }
        for (volatile unsigned int spin = 0; spin < 100000; spin++) {
        }
    }
}

/*
 * qyboot.c — 澜岫开机动画 v3（非对称自然构图美术重做）
 * 直接写 /dev/fb0 (DRM fbdev emulation 提供)：
 *   深蓝渐变 #0A0E1A -> #101826 + 确定性噪声纹理背景
 *   非对称山脊（3 段折线分段浮现，基线微斜）+ 两道椭圆弧涟漪（扩散淡出）
 *   + 上升三角 + 5x7 点阵 LANXIU 淡入 + 左下角橙色增长进度条
 *   纯 framebuffer 直写，抗锯齿/辉光均实时混合，无新依赖（仅 math.h）
 * 由 qyinit 最先启动 (qyboot.unit)，qydesktop 起来后向其发 SIGTERM，动画淡出退出
 * gcc --sysroot=<sysroot> qyboot.c -o qyboot -O2
 */
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <time.h>
#include <math.h>

static volatile sig_atomic_t g_stop = 0;
static void on_term(int s) { (void)s; g_stop = 1; }

static uint32_t *g_fb;
static int g_fd = -1;
static int W, H, LW;
static struct fb_var_screeninfo g_vi;
static int8_t *g_noise;   /* 每像素亮度抖动 [-4,4]，确定性 */

static void fb_open(void)
{
    g_fd = open("/dev/fb0", O_RDWR);
    if (g_fd < 0) _exit(0);
    if (ioctl(g_fd, FBIOGET_VSCREENINFO, &g_vi) < 0) _exit(0);
    struct fb_fix_screeninfo fi;
    ioctl(g_fd, FBIOGET_FSCREENINFO, &fi);
    W = g_vi.xres; H = g_vi.yres; LW = fi.line_length / 4;
    size_t sz = (size_t)fi.line_length * g_vi.yres;
    g_fb = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (g_fb == MAP_FAILED) _exit(0);

    /* 确定性噪声纹理：固定种子，每像素亮度抖动 ±4 */
    g_noise = malloc((size_t)W * H);
    if (!g_noise) _exit(0);
    srand(0x5EED);
    for (size_t i = 0; i < (size_t)W * H; i++)
        g_noise[i] = (int8_t)(rand() % 9 - 4);
}

static inline uint32_t rgb_pack(int r, int g, int b)
{
    return (uint32_t)r << g_vi.red.offset |
           (uint32_t)g << g_vi.green.offset |
           (uint32_t)b << g_vi.blue.offset;
}

static inline void px(int x, int y, int r, int g, int b)
{
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    g_fb[(uint32_t)y * LW + x] = rgb_pack(r, g, b);
}

/* 以 alpha(0-255) 将颜色混合到已绘制的背景上 */
static inline void blend_px(int x, int y, int r, int g, int b, int a)
{
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || a <= 0) return;
    uint32_t o = (uint32_t)y * LW + x;
    if (a >= 255) { g_fb[o] = rgb_pack(r, g, b); return; }
    uint32_t v = g_fb[o];
    int br = (int)((v >> g_vi.red.offset) & 0xff);
    int bg = (int)((v >> g_vi.green.offset) & 0xff);
    int bb = (int)((v >> g_vi.blue.offset) & 0xff);
    int ia = 255 - a;
    br = (r * a + br * ia + 127) / 255;
    bg = (g * a + bg * ia + 127) / 255;
    bb = (b * a + bb * ia + 127) / 255;
    g_fb[o] = rgb_pack(br, bg, bb);
}

static void rect(int x0, int y0, int x1, int y1, int r, int g, int b)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            px(x, y, r, g, b);
}

/* 抗锯齿粗线：包围盒内按到线段的距离做边缘混合 */
static void aa_line(int x0, int y0, int x1, int y1, int half,
                   int r, int g, int b, int alpha)
{
    float fx0 = (float)x0, fy0 = (float)y0;
    float fx1 = (float)x1, fy1 = (float)y1;
    float dx = fx1 - fx0, dy = fy1 - fy0;
    float len2 = dx * dx + dy * dy;
    int minx = x0 < x1 ? x0 : x1, maxx = x0 > x1 ? x0 : x1;
    int miny = y0 < y1 ? y0 : y1, maxy = y0 > y1 ? y0 : y1;
    minx -= half + 1; maxx += half + 1;
    miny -= half + 1; maxy += half + 1;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= W) maxx = W - 1;
    if (maxy >= H) maxy = H - 1;

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float t = 0.0f;
            if (len2 > 0.0001f)
                t = (((float)x - fx0) * dx + ((float)y - fy0) * dy) / len2;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            float ex = fx0 + dx * t - (float)x;
            float ey = fy0 + dy * t - (float)y;
            float dist = sqrtf(ex * ex + ey * ey);
            float cov = (float)half + 0.5f - dist;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            blend_px(x, y, r, g, b, (int)(alpha * cov));
        }
    }
}

/*
 * 抗锯齿椭圆环（涟漪）。
 * biased=1 时只画左下偏向的椭圆弧（下半 + 左半区域，即 ny>=0 或 nx<=0），
 * 形成非对称自然构图；x 半径 > y 半径。
 */
static void aa_ellipse_ring(int cx, int cy, float rx, float ry, int half,
                            int r, int g, int b, int alpha, int biased)
{
    if (rx < 2.0f || ry < 2.0f || alpha <= 0) return;
    int x0 = (int)(cx - rx - half - 2), x1 = (int)(cx + rx + half + 2);
    int y0 = (int)(cy - ry - half - 2), y1 = (int)(cy + ry + half + 2);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y1 >= H) y1 = H - 1;
    float inv_rx = 1.0f / rx, inv_ry = 1.0f / ry;

    for (int y = y0; y <= y1; y++) {
        float nyf = ((float)y - cy) * inv_ry;
        for (int x = x0; x <= x1; x++) {
            float nxf = ((float)x - cx) * inv_rx;
            if (biased && !(nyf >= 0.0f || nxf <= 0.0f)) continue;
            float d = sqrtf(nxf * nxf + nyf * nyf);
            if (d < 0.5f || d > 1.5f) continue;
            /* 椭圆局部尺度：把归一化距离差换算成像素距离 */
            float denom = sqrtf((rx * nxf) * (rx * nxf) + (ry * nyf) * (ry * nyf));
            float scale = denom > 0.0001f ? (rx * ry * d) / denom : 1.0f;
            float dist = fabsf(d - 1.0f) * scale;
            float cov = (float)half + 0.5f - dist;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            blend_px(x, y, r, g, b, (int)(alpha * cov));
        }
    }
}

/* 柔和辉光：宽椭圆环，alpha 随距中心线距离二次衰减 */
static void draw_glow(int cx, int cy, float rx, float ry, int soft,
                      int r, int g, int b, int amax)
{
    if (rx < 2.0f || ry < 2.0f || amax <= 0) return;
    int x0 = (int)(cx - rx - soft - 1), x1 = (int)(cx + rx + soft + 1);
    int y0 = (int)(cy - ry - soft - 1), y1 = (int)(cy + ry + soft + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y1 >= H) y1 = H - 1;
    float inv_rx = 1.0f / rx, inv_ry = 1.0f / ry;
    float fsoft = (float)soft;

    for (int y = y0; y <= y1; y++) {
        float nyf = ((float)y - cy) * inv_ry;
        for (int x = x0; x <= x1; x++) {
            float nxf = ((float)x - cx) * inv_rx;
            float d = sqrtf(nxf * nxf + nyf * nyf);
            if (d < 0.3f || d > 1.7f) continue;
            float denom = sqrtf((rx * nxf) * (rx * nxf) + (ry * nyf) * (ry * nyf));
            float scale = denom > 0.0001f ? (rx * ry * d) / denom : 1.0f;
            float dist = fabsf(d - 1.0f) * scale;
            if (dist >= fsoft) continue;
            float fall = 1.0f - dist / fsoft;
            int a = (int)(amax * fall * fall);
            if (a > 0) blend_px(x, y, r, g, b, a);
        }
    }
}

/* 抗锯齿填充三角形：按三条边的符号距离做边缘混合 */
static void fill_tri_aa(float x0, float y0, float x1, float y1, float x2, float y2,
                        int r, int g, int b, int alpha)
{
    float e0x = x1 - x0, e0y = y1 - y0;
    float e1x = x2 - x1, e1y = y2 - y1;
    float e2x = x0 - x2, e2y = y0 - y2;
    float l0 = sqrtf(e0x * e0x + e0y * e0y);
    float l1 = sqrtf(e1x * e1x + e1y * e1y);
    float l2 = sqrtf(e2x * e2x + e2y * e2y);
    if (l0 < 0.001f || l1 < 0.001f || l2 < 0.001f) return;
    float area = e0x * (y2 - y0) - e0y * (x2 - x0);
    float sgn = area >= 0.0f ? 1.0f : -1.0f;

    int minx = (int)floorf(fminf(fminf(x0, x1), x2)) - 1;
    int maxx = (int)ceilf(fmaxf(fmaxf(x0, x1), x2)) + 1;
    int miny = (int)floorf(fminf(fminf(y0, y1), y2)) - 1;
    int maxy = (int)ceilf(fmaxf(fmaxf(y0, y1), y2)) + 1;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= W) maxx = W - 1;
    if (maxy >= H) maxy = H - 1;

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float d0 = sgn * (e0x * (y - y0) - e0y * (x - x0)) / l0;
            float d1 = sgn * (e1x * (y - y1) - e1y * (x - x1)) / l1;
            float d2 = sgn * (e2x * (y - y2) - e2y * (x - x2)) / l2;
            float mind = fminf(fminf(d0, d1), d2);
            if (mind < -1.0f) continue;
            float cov = mind + 0.5f;
            if (cov > 1.0f) cov = 1.0f;
            blend_px(x, y, r, g, b, (int)(alpha * cov));
        }
    }
}

/* 5x7 点阵字库（LANXIU 顺序），bit4 = 最左列 */
static const uint8_t FONT5x7[6][7] = {
    /* L */ {16, 16, 16, 16, 16, 16, 31},
    /* A */ {14, 17, 17, 17, 31, 17, 17},
    /* N */ {17, 25, 21, 19, 17, 17, 17},
    /* X */ {17, 17, 10,  4, 10, 17, 17},
    /* I */ {31,  4,  4,  4,  4,  4, 31},
    /* U */ {17, 17, 17, 17, 17, 17, 14},
};
static const char LOGO_TEXT[] = "LANXIU";

static void draw_text_5x7(int x, int y, int scale, int r, int g, int b, int alpha)
{
    if (alpha <= 0) return;
    for (int ci = 0; LOGO_TEXT[ci]; ci++) {
        const uint8_t *glyph = FONT5x7[ci];
        for (int row = 0; row < 7; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 5; col++) {
                if (!(bits & (1u << (4 - col)))) continue;
                int bx = x + col * scale, by = y + row * scale;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        blend_px(bx + dx, by + dy, r, g, b, alpha);
            }
        }
        x += (5 + 2) * scale;
    }
}

/* 山脊 3 段折线按总长度比例分段浮现 */
static void draw_ridge_reveal(float x0, float y0, float x1, float y1,
                              float x2, float y2, float x3, float y3,
                              float frac, int r, int g, int b)
{
    float l01 = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    float l12 = sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
    float l23 = sqrtf((x3 - x2) * (x3 - x2) + (y3 - y2) * (y3 - y2));
    float total = l01 + l12 + l23;
    float t = frac * total;
    if (t <= 0.0f) return;
    int half = 2;

    if (t < l01) {
        float k = t / l01;
        aa_line((int)x0, (int)y0,
                (int)(x0 + (x1 - x0) * k), (int)(y0 + (y1 - y0) * k),
                half, r, g, b, 255);
    } else if (t < l01 + l12) {
        aa_line((int)x0, (int)y0, (int)x1, (int)y1, half, r, g, b, 255);
        float k = (t - l01) / l12;
        aa_line((int)x1, (int)y1,
                (int)(x1 + (x2 - x1) * k), (int)(y1 + (y2 - y1) * k),
                half, r, g, b, 255);
    } else {
        aa_line((int)x0, (int)y0, (int)x1, (int)y1, half, r, g, b, 255);
        aa_line((int)x1, (int)y1, (int)x2, (int)y2, half, r, g, b, 255);
        float k = (t - l01 - l12) / l23;
        aa_line((int)x2, (int)y2,
                (int)(x2 + (x3 - x2) * k), (int)(y2 + (y3 - y2) * k),
                half, r, g, b, 255);
    }
}

int main(void)
{
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    fb_open();

    /* 非对称布局：logo 中心偏左；山脊基线微斜（两端 y 差约 H*0.02） */
    int cx = (int)(W * 0.36f);
    int cy = (int)(H * 0.42f);
    int s = H / 5;
    if (s < 24) s = 24;

    int base_y = cy + s / 3;
    int baseL = base_y - H / 100;
    int baseR = base_y + H / 100;

    float P0x = cx - s * 1.15f,        P0y = (float)baseL;
    float P1x = cx - s * 0.28f,        P1y = (float)(baseL - (int)(s * 0.95f));
    float P2x = cx + s * 0.45f,        P2y = (float)(baseR - (int)(s * 0.45f));
    float P3x = cx + s * 1.15f,        P3y = (float)baseR;

    /* 涟漪/辉光中心：logo 中心向左下微偏 */
    int rcx = cx - s / 8;
    int rcy = cy - s / 16;

    int f = 0;
    while (!g_stop) {
        int fr = f % 150;   /* 每帧 50ms，150 帧 = 7.5s 循环 */

        /* 背景：深蓝渐变 #0A0E1A -> #101826 + 噪声亮度抖动 ±4 */
        for (int y = 0; y < H; y++) {
            int gr = 10 + (6 * y) / H;
            int gg = 14 + (10 * y) / H;
            int gb = 26 + (12 * y) / H;
            uint32_t base = (uint32_t)y * LW;
            for (int x = 0; x < W; x++) {
                int n = g_noise[base + x];
                g_fb[base + x] = rgb_pack(gr + n, gg + n, gb + n);
            }
        }

        /* 辉光：暖橙外环低 alpha，恒定柔和 */
        draw_glow(rcx, rcy, s * 1.35f, s * 0.95f, s / 4, 255, 176, 122, 26);

        /* 涟漪：两道椭圆弧扩散并淡出（1s-3s） */
        if (fr >= 20) {
            float q = (fr - 20) / 40.0f;
            if (q > 1.0f) q = 1.0f;
            float rad1 = s * (0.30f + 1.05f * q);
            int a1 = (int)(235.0f * (1.0f - q) + 25.0f);
            aa_ellipse_ring(rcx, rcy, rad1 * 1.28f, rad1 * 0.70f, 2,
                            255, 92, 26, a1, 1);
            float q2 = (fr - 24) / 36.0f;
            if (q2 < 0.0f) q2 = 0.0f; else if (q2 > 1.0f) q2 = 1.0f;
            float rad2 = s * (0.16f + 0.78f * q2);
            int a2 = (int)(180.0f * (1.0f - q2) + 20.0f);
            aa_ellipse_ring(rcx, rcy, rad2 * 1.28f, rad2 * 0.70f, 2,
                            255, 92, 26, a2, 1);
        }

        /* 山脊：3 段折线分段浮现（0-0.8s） */
        float p = fr / 16.0f;
        if (p > 1.0f) p = 1.0f;
        draw_ridge_reveal(P0x, P0y, P1x, P1y, P2x, P2y, P3x, P3y, p,
                          242, 244, 248);

        /* 三角：从山脊上升（1.5s-2.8s） */
        if (fr >= 30) {
            float q = (fr - 30) / 26.0f;
            if (q > 1.0f) q = 1.0f;
            float apex_x = cx - s * 0.10f;
            float top_y = (float)(base_y - s * 0.18f) - q * s * 0.78f;
            float hw = s * 0.26f, hh = s * 0.46f;
            int alpha = 255;
            if (q < 0.15f) alpha = (int)(255.0f * (q / 0.15f));
            fill_tri_aa(apex_x, top_y,
                        apex_x - hw, top_y + hh,
                        apex_x + hw, top_y + hh,
                        255, 92, 26, alpha);
        }

        /* LANXIU：5x7 点阵字右下淡入（2.5s-4s） */
        if (fr >= 50) {
            float q = (fr - 50) / 30.0f;
            if (q > 1.0f) q = 1.0f;
            int tscale = H / 350;
            if (tscale < 1) tscale = 1;
            if (tscale > 4) tscale = 4;
            draw_text_5x7((int)(W * 0.72f), (int)(H * 0.78f), tscale,
                          232, 236, 244, (int)(255.0f * q));
        }

        /* 进度条：左下角，橙色 0→100% */
        {
            int bx0 = (int)(W * 0.06f);
            int by  = (int)(H * 0.90f);
            int bw  = (int)(W * 0.34f);
            int bh  = 6;
            float prog = fr / 150.0f;
            rect(bx0 - 1, by - 1, bx0 + bw + 1, by + bh + 1, 30, 40, 60);
            rect(bx0, by, bx0 + bw, by + bh, 18, 26, 44);
            int pw = (int)(bw * prog);
            if (pw > 0) rect(bx0, by, bx0 + pw, by + bh, 255, 92, 26);
        }

        usleep(50000);
        f++;
    }

    /* 淡出: 40 帧叠加变黑 */
    for (int d = 0; d < 40; d++) {
        for (int y = 0; y < H; y++) {
            uint32_t base = (uint32_t)y * LW;
            for (int x = 0; x < W; x++) {
                uint32_t o = base + x;
                uint32_t v = g_fb[o];
                uint32_t r = (v >> g_vi.red.offset) & 0xff;
                uint32_t g = (v >> g_vi.green.offset) & 0xff;
                uint32_t b = (v >> g_vi.blue.offset) & 0xff;
                r = r * 19 / 20; g = g * 19 / 20; b = b * 19 / 20;
                g_fb[o] = r << g_vi.red.offset | g << g_vi.green.offset | b << g_vi.blue.offset;
            }
        }
        usleep(50000);
    }
    return 0;
}
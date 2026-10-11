/*
 * qyboot.c — 澜岫开机动画 v3（美术重做）
 * 直接写 /dev/fb0 (DRM fbdev emulation 提供)，无新依赖：
 *   - 深蓝垂直渐变 #0A0E1A→#101826 + 确定性噪声纹理（±4 亮度抖动）
 *   - 非对称自然构图：logo 中心 (0.36W, 0.42H)，山脊基线微斜约 6°
 *   - 白色山脊（4 段折线、抗锯齿，分段浮现）
 *   - 橙色涟漪（椭圆弧 + #FFB07A 辉光，从山脊上方向左下不对称扩散）
 *   - 橙色三角从主峰上升（抗锯齿填充）
 *   - 右下 editorial 排版：5x7 点阵 LANXIU + 「澜岫」几何字形竖排 + 橙色细线
 *   - 左下角非居中进度条（品牌橙填充 + 外辉光 + 半透明橙边框）
 * 帧时序（0..150 帧循环，每帧 usleep 50000，约 7.5s 循环）：
 *   山脊分段浮现 0-24 · 涟漪扩散 30-90 · 三角上升 45-84
 *   文字淡入 75-120 · 进度条 0-100%（0-150）
 * SIGTERM 后 40 帧线性淡出变黑。
 * 由 qyinit 最先启动 (qyboot.unit)，qydesktop 起来后向其发 SIGTERM。
 * gcc --sysroot=<sysroot> qyboot.c -o qyboot -O2 -lm
 */
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

static volatile sig_atomic_t g_stop = 0;
static void on_term(int s) { (void)s; g_stop = 1; }

static uint32_t *g_fb;
static int g_fd = -1;
static int W, H, LW;
static struct fb_var_screeninfo g_vi;

static void fb_open(void)
{
    g_fd = open("/dev/fb0", O_RDWR);
    if (g_fd < 0) _exit(0);
    if (ioctl(g_fd, FBIOGET_VSCREENINFO, &g_vi) < 0) _exit(0);
    struct fb_fix_screeninfo fi;
    if (ioctl(g_fd, FBIOGET_FSCREENINFO, &fi) < 0) _exit(0);
    W = g_vi.xres; H = g_vi.yres; LW = fi.line_length / 4;
    size_t sz = (size_t)fi.line_length * g_vi.yres;
    g_fb = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (g_fb == MAP_FAILED) _exit(0);
}

static inline uint32_t fb_pack(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint32_t)r << g_vi.red.offset |
           (uint32_t)g << g_vi.green.offset |
           (uint32_t)b << g_vi.blue.offset;
}

static inline void px(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    g_fb[(uint32_t)y * LW + x] = fb_pack(r, g, b);
}

/* alpha 混合（over 语义：src 在上，读回当前像素做混合） */
static inline void px_blend(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (x < 0 || y < 0 || x >= W || y >= H || a == 0) return;
    if (a == 255) { px(x, y, r, g, b); return; }
    uint32_t o = (uint32_t)y * LW + x;
    uint32_t v = g_fb[o];
    uint8_t vr = (uint8_t)((v >> g_vi.red.offset) & 0xff);
    uint8_t vg = (uint8_t)((v >> g_vi.green.offset) & 0xff);
    uint8_t vb = (uint8_t)((v >> g_vi.blue.offset) & 0xff);
    g_fb[o] = fb_pack((uint8_t)((r * a + vr * (255 - a)) / 255),
                      (uint8_t)((g * a + vg * (255 - a)) / 255),
                      (uint8_t)((b * a + vb * (255 - a)) / 255));
}

static void rect(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b)
{
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1; if (y1 >= H) y1 = H - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            px(x, y, r, g, b);
}

static void rect_blend(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            px_blend(x, y, r, g, b, a);
}

/* ---------- 背景：深蓝渐变 + 确定性噪声 ---------- */
/* 同一坐标每次同值，亮度抖动 -4..+4 */
static int noise_at(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + 0x4a7b9cu;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (int)(h % 9) - 4;
}

static void draw_background(void)
{
    for (int y = 0; y < H; y++) {
        float k = H > 1 ? (float)y / (H - 1) : 0;
        uint8_t r = (uint8_t)(10 + 6 * k);   /* #0A0E1A(10,14,26) */
        uint8_t g = (uint8_t)(14 + 10 * k);   /*  → #101826(16,24,38) */
        uint8_t b = (uint8_t)(26 + 12 * k);
        for (int x = 0; x < W; x++) {
            int n = noise_at(x, y);
            px(x, y, (uint8_t)(r + n), (uint8_t)(g + n), (uint8_t)(b + n));
        }
    }
}

/* ---------- 抗锯齿粗线（按到线段的距离混合半透明） ---------- */
static void aa_line(float x0, float y0, float x1, float y1, float w,
                    uint8_t r, uint8_t g, uint8_t b)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    if (len2 < 0.0001f) return;
    float half = w * 0.5f;
    int minx = (int)floorf(fminf(x0, x1) - half - 1);
    int maxx = (int)ceilf(fmaxf(x0, x1) + half + 1);
    int miny = (int)floorf(fminf(y0, y1) - half - 1);
    int maxy = (int)ceilf(fmaxf(y0, y1) + half + 1);
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float pvx = x + 0.5f - x0, pvy = y + 0.5f - y0;
            float t = (pvx * dx + pvy * dy) / len2;
            if (t < 0) t = 0; else if (t > 1) t = 1;
            float cx = pvx - dx * t, cy = pvy - dy * t;
            float d = sqrtf(cx * cx + cy * cy);
            float alpha;
            if (d <= half - 0.5f) alpha = 1.0f;
            else if (d >= half + 0.5f) alpha = 0.0f;
            else alpha = half + 0.5f - d;
            if (alpha > 0.01f)
                px_blend(x, y, r, g, b, (uint8_t)(alpha * 255));
        }
    }
}

/* ---------- 椭圆弧环（抗锯齿；rot 弧度；a0/a1 度，屏幕 y 向下：
 * 0°=右、90°=下、-90°=上、±180°=左。135°→270° 覆盖左下→左→上，开口朝右） */
static void ellipse_arc(float cx, float cy, float rx, float ry, float rot,
                        float a0, float a1, float th,
                        uint8_t r, uint8_t g, uint8_t b, float alpha_scale)
{
    if (rx < 1 || ry < 1) return;
    float cosr = cosf(rot), sinr = sinf(rot);
    int minx = (int)floorf(cx - rx - th - 2), maxx = (int)ceilf(cx + rx + th + 2);
    int miny = (int)floorf(cy - ry - th - 2), maxy = (int)ceilf(cy + ry + th + 2);
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float xr = dx * cosr + dy * sinr;
            float yr = -dx * sinr + dy * cosr;
            float ang = atan2f(yr, xr) * (180.0f / (float)M_PI);
            if (ang < 0) ang += 360.0f;
            if (ang < a0 || ang > a1) continue;
            float f = (xr * xr) / (rx * rx) + (yr * yr) / (ry * ry);
            float gx = 2.0f * xr / (rx * rx);
            float gy = 2.0f * yr / (ry * ry);
            float grad = sqrtf(gx * gx + gy * gy);
            if (grad < 0.0001f) continue;
            float dist = fabsf(f - 1.0f) / grad;   /* 近似像素距离 */
            float alpha = th * 0.5f + 0.5f - dist;
            if (alpha <= 0) continue;
            if (alpha > 1) alpha = 1;
            px_blend(x, y, r, g, b, (uint8_t)(alpha * 255 * alpha_scale));
        }
    }
}

/* ---------- 抗锯齿填充三角形（边缘按距离混合） ---------- */
static float edge_sdist(float ax, float ay, float bx, float by, float px, float py)
{
    float abx = bx - ax, aby = by - ay;
    float apx = px - ax, apy = py - ay;
    float cross = abx * apy - aby * apx;
    float len = sqrtf(abx * abx + aby * aby);
    if (len < 0.0001f) return 0.0f;
    return cross / len;
}

static void aa_triangle(float x0, float y0, float x1, float y1, float x2, float y2,
                        uint8_t r, uint8_t g, uint8_t b, float alpha_scale)
{
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    float sgn = area >= 0 ? 1.0f : -1.0f;
    int minx = (int)floorf(fminf(fminf(x0, x1), x2)) - 1;
    int maxx = (int)ceilf(fmaxf(fmaxf(x0, x1), x2)) + 1;
    int miny = (int)floorf(fminf(fminf(y0, y1), y2)) - 1;
    int maxy = (int)ceilf(fmaxf(fmaxf(y0, y1), y2)) + 1;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float pvx = x + 0.5f, pvy = y + 0.5f;
            float d0 = edge_sdist(x0, y0, x1, y1, pvx, pvy) * sgn;
            float d1 = edge_sdist(x1, y1, x2, y2, pvx, pvy) * sgn;
            float d2 = edge_sdist(x2, y2, x0, y0, pvx, pvy) * sgn;
            float dm = fminf(d0, fminf(d1, d2));
            float alpha;
            if (dm >= 0.5f) alpha = 1.0f;
            else if (dm <= -0.5f) alpha = 0.0f;
            else alpha = dm + 0.5f;
            if (alpha > 0.01f)
                px_blend(x, y, r, g, b, (uint8_t)(alpha * 255 * alpha_scale));
        }
    }
}

/* ---------- 5x7 点阵字（bit4 = 最左列） ---------- */
static const uint8_t FONT_LANXIU[6][7] = {
    {16, 16, 16, 16, 16, 16, 31}, /* L */
    {14, 17, 17, 31, 17, 17, 17}, /* A */
    {17, 25, 21, 19, 17, 17, 17}, /* N */
    {17, 17, 10, 4, 10, 17, 17},  /* X */
    {31, 4, 4, 4, 4, 4, 31},      /* I */
    {17, 17, 17, 17, 17, 17, 14}, /* U */
};

/* 「澜岫」5x5 几何字形（bit4 = 最左列），竖排 */
static const uint8_t GLYPH_LAN[5] = {23, 13, 23, 13, 23}; /* 氵+阑 简化 */
static const uint8_t GLYPH_XIU[5] = {23, 21, 31, 13, 13}; /* 山+由 简化 */

static void draw_glyph5(int x, int y, int dot, const uint8_t rows[5],
                        uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
            if (rows[i] & (1 << (4 - j)))
                rect_blend(x + j * dot, y + i * dot,
                           x + j * dot + dot - 1, y + i * dot + dot - 1,
                           r, g, b, alpha);
}

static void draw_text5x7(int x, int y, int dot, const uint8_t rows[7],
                        uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 5; j++)
            if (rows[i] & (1 << (4 - j)))
                rect_blend(x + j * dot, y + i * dot,
                           x + j * dot + dot - 1, y + i * dot + dot - 1,
                           r, g, b, alpha);
}

/* 右下 editorial 排版：LANXIU 点阵 + 细线 + 「澜岫」竖排 */
static void draw_text(float alpha_f)
{
    if (alpha_f <= 0) return;
    uint8_t alpha = (uint8_t)(alpha_f * 255);
    int dot = H / 160;
    if (dot < 2) dot = 2;
    if (dot > 8) dot = 8;
    int x0 = (int)(W * 0.72f);
    int y0 = (int)(H * 0.78f);
    int pitch = 6 * dot; /* 每字 5 列 + 1 列字距 */
    for (int i = 0; i < 6; i++)
        draw_text5x7(x0 + i * pitch, y0, dot, FONT_LANXIU[i], 255, 92, 26, alpha);
    int total_w = 6 * pitch - dot;
    /* editorial 细线：位于字阵下方 */
    int line_y = y0 + 7 * dot + dot;
    rect_blend(x0, line_y, x0 + total_w, line_y + (dot > 3 ? 2 : 1),
               255, 92, 26, (uint8_t)(alpha * 0.85f));
    /* 「澜岫」竖排：在 LANXIU 右侧 */
    int gx = x0 + total_w + 2 * dot;
    draw_glyph5(gx, y0, dot, GLYPH_LAN, 255, 92, 26, alpha);
    draw_glyph5(gx, y0 + 6 * dot, dot, GLYPH_XIU, 255, 92, 26, alpha);
}

/* ---------- 白色山脊：4 段折线，基线微斜约 6°，分段浮现 ---------- */
static void draw_ridge(float cx, float cy, float s, float p)
{
    if (p <= 0) return;
    float base_y = cy + s * 0.33f;
    float tilt = 0.105f; /* tan(6°)≈0.105 */
    float pts[5][2];
    pts[0][0] = cx - s;         pts[0][1] = base_y - tilt * s;
    pts[1][0] = cx - 0.45f * s;   pts[1][1] = base_y - tilt * 0.45f * s - 0.75f * s;
    pts[2][0] = cx + 0.05f * s;  pts[2][1] = base_y + tilt * 0.05f * s - 0.28f * s;
    pts[3][0] = cx + 0.60f * s;   pts[3][1] = base_y + tilt * 0.60f * s - 0.90f * s;
    pts[4][0] = cx + s;          pts[4][1] = base_y + tilt * s;
    int nseg = 4;
    for (int i = 0; i < nseg; i++) {
        float st = i / (float)nseg, en = (i + 1) / (float)nseg;
        if (p <= st) continue;
        float sub = fminf(1.0f, (p - st) / (en - st));
        float ex = pts[i][0] + (pts[i + 1][0] - pts[i][0]) * sub;
        float ey = pts[i][1] + (pts[i + 1][1] - pts[i][1]) * sub;
        aa_line(pts[i][0], pts[i][1], ex, ey, 3.0f, 242, 244, 248); /* #F2F4F8 */
    }
}

/* ---------- 橙色涟漪：两道椭圆弧向左下扩散（半径渐大、透明度渐低） ---------- */
static void draw_ripples(float cx, float cy, float s, float p)
{
    if (p <= 0) return;
    float base_y = cy + s * 0.33f;
    float rcx = cx - 0.10f * s;          /* 中心位于山脊上方偏左 */
    float rcy = base_y - 0.95f * s;
    float pr = fminf(1.0f, p);
    /* 外环 */
    float rx1 = s * (0.20f + 0.78f * pr);
    float ry1 = rx1 * 0.55f;
    float a1 = 0.85f * (1.0f - pr) + 0.08f;
    ellipse_arc(rcx, rcy, rx1 + 3, ry1 + 2, -0.16f, 135, 270, 6.0f,
                255, 176, 122, a1 * 0.32f); /* #FFB07A 辉光外晕 */
    ellipse_arc(rcx, rcy, rx1, ry1, -0.16f, 135, 270, 2.0f,
                255, 92, 26, a1);            /* #FF5C1A 主环 */
    /* 内环 */
    float rx2 = s * (0.08f + 0.55f * pr);
    float ry2 = rx2 * 0.55f;
    float a2 = 0.70f * (1.0f - pr) + 0.08f;
    ellipse_arc(rcx, rcy, rx2 + 3, ry2 + 2, 0.12f, 135, 270, 5.0f,
                255, 176, 122, a2 * 0.30f);
    ellipse_arc(rcx, rcy, rx2, ry2, 0.12f, 135, 270, 2.0f,
                255, 92, 26, a2);
}

/* ---------- 橙色三角：从主峰升起 ---------- */
static void draw_triangle(float cx, float cy, float s, float p)
{
    if (p <= 0) return;
    float tp = fminf(1.0f, p);
    float base_y = cy + s * 0.33f;
    float tx = cx + 0.60f * s;                             /* 主峰 x */
    float py = base_y + 0.105f * 0.60f * s - 0.90f * s;  /* 主峰 y */
    float tby = py - s * 0.95f * tp;                    /* 底边随进度上升 */
    float h = s * 0.44f, w = s * 0.26f;
    float alpha = fminf(1.0f, tp * 2.0f);              /* 快速淡入 */
    aa_triangle(tx, tby - h, tx - w, tby, tx + w, tby,
                255, 92, 26, alpha);                      /* #FF5C1A */
}

/* ---------- 左下角进度条：品牌橙填充 + 外辉光 + 半透明橙边框 ---------- */
static void draw_progress(float t)
{
    int x0 = (int)(W * 0.06f);
    int y0 = (int)(H * 0.90f);
    int bw = (int)(W * 0.34f);
    int bh = 6;
    if (bw < 10) return;
    rect(x0, y0, x0 + bw, y0 + bh, 16, 24, 42);            /* 深色底槽 */
    for (int x = x0; x <= x0 + bw; x++) {                  /* 半透明橙边框 */
        px_blend(x, y0, 255, 92, 26, 55);
        px_blend(x, y0 + bh, 255, 92, 26, 55);
    }
    for (int y = y0; y <= y0 + bh; y++) {
        px_blend(x0, y, 255, 92, 26, 55);
        px_blend(x0 + bw, y, 255, 92, 26, 55);
    }
    int pw = (int)(bw * t);
    if (pw <= 0) return;
    /* 外辉光：围绕填充部分的柔光 */
    for (int dy = -4; dy <= bh + 3; dy++) {
        for (int dx = -4; dx <= pw + 3; dx++) {
            int ddx = dx < 0 ? -dx : (dx > pw ? dx - pw : 0);
            int ddy = dy < 0 ? -dy : (dy > bh ? dy - bh : 0);
            float dist = sqrtf((float)(ddx * ddx + ddy * ddy));
            float a = (1.0f - dist / 4.0f) * 0.28f;
            if (a > 0)
                px_blend(x0 + dx, y0 + dy, 255, 92, 26, (uint8_t)(a * 255));
        }
    }
    rect(x0, y0, x0 + pw, y0 + bh, 255, 92, 26);           /* 品牌橙填充 */
}

int main(void)
{
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    fb_open();

    int f = 0;
    while (!g_stop) {
        int fr = f % 150;
        float t = fr / 150.0f;

        draw_background();

        float cx = W * 0.36f, cy = H * 0.42f;
        float s = H / 5.0f;

        /* 山脊分段浮现 0-24 */
        draw_ridge(cx, cy, s, fr < 24 ? fr / 24.0f : 1.0f);
        /* 涟漪扩散 30-90 */
        if (fr >= 30)
            draw_ripples(cx, cy, s, (fr - 30) / 60.0f);
        /* 三角上升 45-84（之后保持） */
        if (fr >= 45)
            draw_triangle(cx, cy, s, (fr - 45) / 39.0f);
        /* 文字淡入 75-120（之后保持） */
        if (fr >= 75)
            draw_text(fminf(1.0f, (fr - 75) / 45.0f));
        /* 进度条 0-100% */
        draw_progress(t);

        usleep(50000);
        f++;
    }

    /* 淡出：40 帧线性变黑 */
    for (int d = 0; d < 40; d++) {
        float k = 1.0f - d / 39.0f;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                uint32_t o = (uint32_t)y * LW + x;
                uint32_t v = g_fb[o];
                uint8_t r = (uint8_t)((v >> g_vi.red.offset) & 0xff);
                uint8_t g = (uint8_t)((v >> g_vi.green.offset) & 0xff);
                uint8_t b = (uint8_t)((v >> g_vi.blue.offset) & 0xff);
                g_fb[o] = fb_pack((uint8_t)(r * k), (uint8_t)(g * k), (uint8_t)(b * k));
            }
        }
        usleep(50000);
    }
    return 0;
}
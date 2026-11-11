#include "display.h"
#include "display_internal.h"
#include "servo_head.h"

#include "esp_timer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Small, deterministic software renderer for the 128x64 SSD1306.
// Keep the existing XiaoZhi display driver and the three public face states.
static int g_draw_offset_x = 0;
static int g_draw_offset_y = 0;

static void pixel(int x, int y, bool on = true)
{
    x += g_draw_offset_x;
    y += g_draw_offset_y;
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    uint8_t *buffer = display_framebuffer();
    uint8_t &b = buffer[x + (y >> 3) * OLED_WIDTH];
    const uint8_t mask = (uint8_t)(1U << (y & 7));
    if (on) b |= mask;
    else b &= (uint8_t)~mask;
}

static void fill_circle(int cx, int cy, int r, bool on = true)
{
    for (int y = -r; y <= r; ++y) {
        const int q = r * r - y * y;
        const int dx = q > 0 ? (int)sqrtf((float)q) : 0;
        for (int x = -dx; x <= dx; ++x) pixel(cx + x, cy + y, on);
    }
}

static void fill_ellipse(int cx, int cy, int rx, int ry, bool on = true)
{
    for (int y = -ry; y <= ry; ++y) {
        const float ny = (float)y / (float)ry;
        const float inside = 1.0f - ny * ny;
        if (inside < 0.0f) continue;
        const int dx = (int)(rx * sqrtf(inside));
        for (int x = -dx; x <= dx; ++x) pixel(cx + x, cy + y, on);
    }
}

static void line(int x0, int y0, int x1, int y1, int thickness = 1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int oy = 0; oy < thickness; ++oy) pixel(x0, y0 + oy);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int clamp_int(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

static void draw_closed_eye(int cx, int cy, bool sleeping)
{
    // Pixel-friendly curved eyelid; sleeping lids droop more in the center.
    const int half = sleeping ? 16 : 17;
    const int mid = sleeping ? 5 : 3;
    line(cx - half, cy, cx - 11, cy + 2, 2);
    line(cx - 11, cy + 2, cx - 5, cy + mid, 2);
    line(cx - 5, cy + mid, cx + 4, cy + mid, 2);
    line(cx + 4, cy + mid, cx + 11, cy + 2, 2);
    line(cx + 11, cy + 2, cx + half, cy, 2);
    if (sleeping) {
        // Tiny lower lash corners make the sleeping expression softer.
        pixel(cx - half, cy + 1);
        pixel(cx + half, cy + 1);
    }
}

static void draw_eye(int expr, int cx, int cy, int gaze_x, int gaze_y,
                     int eye_shift_x, int eye_shift_y, int step)
{
    cx += eye_shift_x;
    cy += eye_shift_y;

    if (step == 3) {
        draw_closed_eye(cx, cy, true);
        return;
    }
    if (step == 1 || step == 2) {
        draw_closed_eye(cx, cy, false);
        if (step == 2) line(cx - 14, cy - 2, cx + 14, cy - 2, 1);
        return;
    }

    // Preserve the same eye anchors, but let the eyelid silhouette carry
    // some emotion too. No eyebrows or alternate face assets are introduced.
    int eye_rx = 18;
    int eye_ry = 14;
    switch (expr) {
        case 10: eye_rx = 17; eye_ry = 15; break; // surprised: wider/taller eye
        case 11: eye_rx = 18; eye_ry = 13; break; // pleading: slightly softened eye
        case 12: eye_rx = 18; eye_ry = 10; break; // skeptical: narrowed lid
        case 13: eye_rx = 18; eye_ry = 12; break; // sly half-smile
        case 14: eye_rx = 18; eye_ry = 10; break; // unimpressed: relaxed/narrow
        case 15: eye_rx = 18; eye_ry = 12; break; // worried: subtly narrowed
        case 16: eye_rx = 19; eye_ry = 15; break; // happy
        case 17: eye_rx = 18; eye_ry = 8; break;  // playful angry
        default: break;
    }
    fill_ellipse(cx, cy, eye_rx, eye_ry, true);

    const int gx = clamp_int(gaze_x, -7, 7);
    const int gy = clamp_int(gaze_y, -5, 5);
    const int pupil_radius = (expr == 10 || expr == 11) ? 5 : 6;
    const int px = cx + gx;
    const int py = cy + gy;

    fill_circle(px, py, pupil_radius, false);
    pixel(px - 2, py - 3, true);
    pixel(px - 1, py - 3, true);
    pixel(px - 2, py - 2, true);
    fill_circle(px + 2, py + 2, 1, true);
}

static void draw_smile(void)
{
    // Compact base smile: about 18 pixels wide, with 3-pixel strokes.
    static const int xs[] = {55, 57, 59, 61, 64, 67, 69, 71, 73};
    static const int ys[] = {48, 50, 51, 52, 53, 52, 51, 50, 48};
    for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); ++i) {
        if (i + 1 < sizeof(xs) / sizeof(xs[0]))
            line(xs[i], ys[i], xs[i + 1], ys[i + 1], 3);
    }
}

// Thinking moods affect expression and mouth shape only; gaze comes from the servo controller.

static void draw_sleep_zs(void)
{
    // Float the Z symbols through a small, repeating cycle without clearing
    // or redrawing any external UI layer.
    const int phase = (int)((esp_timer_get_time() / 350000LL) % 4LL);
    const int y1 = 10 - phase;
    const int y2 = 5 - (phase / 2);
    line(102, y1, 110, y1, 2);
    line(110, y1, 102, y1 + 7, 2);
    line(102, y1 + 7, 110, y1 + 7, 2);
    line(113, y2, 119, y2, 1);
    line(119, y2, 113, y2 + 6, 1);
    line(113, y2 + 6, 119, y2 + 6, 1);
}

static void draw_mouth(int expr, int step, int motion)
{
    if (step == 3) {
        line(61, 51, 67, 51, 2);
        return;
    }

    switch (expr) {
        case 10: { // ð³ subtle surprised-mouth pulse, synchronized with brow motion
            const int ry = (motion == 1 || motion == 2) ? 5 : 3;
            fill_ellipse(64, 51, 3, ry, true);
            fill_ellipse(64, 51, 1, ry > 3 ? 2 : 1, false);
            return;
        }
        case 11: // ð¥º tiny pleading pout gently trembles
            line(60, 51 + ((motion == 1) ? 1 : 0), 64, 53, 2);
            line(64, 53, 68, 51 + ((motion == 1) ? 1 : 0), 2);
            return;
        case 12: // ð¤¨ restrained, straight skeptical mouth
            line(59, 52 + ((motion == 2) ? 1 : 0), 69, 52, 2);
            return;
        case 13: // ð crooked half-smile lifts slightly at the corner
            line(56, 52, 63, 52, 2);
            line(63, 52, 70, 49 - ((motion == 1 || motion == 2) ? 1 : 0), 2);
            line(70, 49 - ((motion == 1 || motion == 2) ? 1 : 0), 74, 49 - ((motion == 1 || motion == 2) ? 1 : 0), 2);
            return;
        case 14: // ð flat, unimpressed mouth
            line(59, 52, 69, 52, 2);
            return;
        case 15: // ð worried mouth trembles by one pixel
            line(58, 49, 64, 53 + ((motion == 1) ? 1 : 0), 2);
            line(64, 53 + ((motion == 1) ? 1 : 0), 70, 49, 2);
            return;
        case 2: { // Mouth opening follows the actual outgoing PCM level.
            const uint8_t level = face_get_speech_level();
            // Quiet frames keep the mouth nearly closed; louder syllables open
            // progressively. A small time variation prevents a rigid "meter" look.
            if (level < 12) {
                line(59, 50, 69, 50, 2);
            } else if (level < 42) {
                line(59, 50, 64, 52, 2);
                line(64, 52, 69, 50, 2);
            } else if (level < 88) {
                line(59, 49, 64, 53, 2);
                line(64, 53, 69, 49, 2);
            } else if (level < 150) {
                fill_ellipse(64, 51, 4, 3, true);
                fill_ellipse(64, 51, 2, 1, false);
            } else {
                const int pulse = (int)((esp_timer_get_time() / 120000LL) % 2LL);
                fill_ellipse(64, 51, 5, pulse ? 4 : 3, true);
                fill_ellipse(64, 51, 2, 1, false);
            }
            return;
        }
        default:
            if (step == 5) {
                line(59, 51, 63, 53, 2);
                line(63, 53, 68, 51, 2);
                pixel(69, 50);
            } else {
                draw_smile();
            }
            return;
    }
}

// Shared face layer: stable eye anchors and consistent geometry across states.
static void draw_base_face(int expr, int step, int sX, int sY, int eye_y,
                           int gaze_x, int gaze_y,
                           int eye_shift_x, int eye_shift_y, int motion)
{
    // Eyes and mouth are facial anatomy: keep them in the shared face pass.
    // Expression parameters alter their shape, never swap the face identity.
    draw_eye(expr, 35 + sX, eye_y, gaze_x, gaze_y,
             eye_shift_x, eye_shift_y, step);
    draw_eye(expr, 93 + sX, eye_y, gaze_x, gaze_y,
             eye_shift_x, eye_shift_y, step);
    // Mouth follows the same face translation as the eyes (including sleep breathing).
    // Apply the offset only for mouth drawing so the sleep Z overlay stays anchored.
    g_draw_offset_x = sX;
    g_draw_offset_y = sY;
    draw_mouth(expr, step, motion);
    g_draw_offset_x = 0;
    g_draw_offset_y = 0;
}

// State-specific decorations only; facial features remain in the shared base pass.
static void draw_expression_overlay(int step)
{
    if (step == 3) draw_sleep_zs();
}

void display_render_mochi_gaze(
    int expr, int step, int sX, int sY, int gaze_x, int gaze_y,
    int eye_shift_x, int eye_shift_y)
{
    oled_init();
    display_clear_framebuffer();

    const int eye_y = 25 + sY;
    // Shared base-face layer: eye placement and mouth position share the same
    // translation across states. State animation supplies expression and gaze.
    const int motion = (int)((esp_timer_get_time() / 220000LL) % 4LL);
    // Draw the permanent face anatomy first. Every state uses these same eye
    // anchors and the same mouth anchor. Do not add expression-specific gaze
    // offsets here; that would conflict with the shared servo gaze direction.
    draw_base_face(expr, step, sX, sY, eye_y, gaze_x, gaze_y,
                   eye_shift_x, eye_shift_y, motion);

    // Expressions are overlays on that face: mouth shape and sleep symbols can
    // change, while the face identity and its layout remain stable.
    draw_expression_overlay(step);

    display_render_buffer(display_framebuffer());
}

void display_render_mochi(int expr, int step, int sX, int sY, int arahLirik)
{
    int gaze_x = 0, gaze_y = 0;
    switch (arahLirik) {
        case 1: gaze_x = -5; break;
        case 2: gaze_x = 5; break;
        case 3: gaze_y = -4; break;
        default: break;
    }
    display_render_mochi_gaze(expr, step, sX, sY, gaze_x, gaze_y, 0, 0);
}

void face_render(void)
{
    const int64_t now_us = esp_timer_get_time();
    // Natural, infrequent blink: two short frames every ~5.3 seconds.
    // The renderer must be called repeatedly for the animation to be visible.
    const int64_t blink_phase = now_us % 5300000LL;
    const int blink_step = blink_phase < 90000LL ? 1 :
                           (blink_phase < 160000LL ? 2 : 0);

    int gaze_x = 0;
    int gaze_y = 0;
    uint32_t duration_ms = 0;
    uint32_t revision = 0;
    uint32_t start_tick = 0;
    (void)servo_head_get_gaze(&gaze_x, &gaze_y, &duration_ms, &revision, &start_tick);
    const int eye_shift_x = gaze_x * 2;
    const int eye_shift_y = gaze_y * 2;

    switch (face_get_state()) {
        case FACE_THINKING:
            // Expression may change, but gaze always comes from the servo controller.
            display_render_mochi_gaze(0, blink_step ? blink_step : 5,
                                      0, 0, gaze_x, gaze_y,
                                      eye_shift_x, eye_shift_y);
            break;
        case FACE_SPEAKING:
            display_render_mochi_gaze(2, blink_step,
                                      0, 0, gaze_x, gaze_y,
                                      eye_shift_x, eye_shift_y);
            break;
        case FACE_SLEEP:
        default:
            display_render_mochi_gaze(0, 3, 0, 0, gaze_x, gaze_y,
                                      eye_shift_x, eye_shift_y);
            break;
    }
}

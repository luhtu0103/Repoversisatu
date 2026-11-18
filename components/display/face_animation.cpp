#include "display.h"
#include "display_internal.h"
#include "servo_head.h"
#include <math.h>
#include <stdlib.h>
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Architecture: face_base.cpp draws one shared face layout; each state below
// supplies only its expression, gaze, blink and state-specific motion.
// OLED is relatively slow at 400 kHz. 45 ms keeps gaze movement visibly smooth
// without turning the animation task into an aggressive I2C producer.
static constexpr TickType_t FRAME_DELAY = pdMS_TO_TICKS(45);

static uint32_t rnd(uint32_t max_value)
{
    return max_value ? esp_random() % max_value : 0;
}

static bool state_ok(face_state_t state)
{
    return face_get_state() == state;
}

static int rendered_gaze_x = 0;
static int rendered_gaze_y = 0;
static int transition_from_x = 0;
static int transition_from_y = 0;
static int transition_to_x = 0;
static int transition_to_y = 0;
static uint32_t seen_gaze_revision = UINT32_MAX;
static uint32_t transition_start_tick = 0;
static uint32_t transition_duration_ms = 0;
static int prev_eye_x = 0;
static int prev_eye_y = 0;

static void render(
    int expr,
    int step,
    int sx,
    int sy,
    int gaze_x,
    int gaze_y,
    int eye_shift_x = 0,
    int eye_shift_y = 0)
{
    // All face states use the servo's shared direction as the only gaze source.
    // Interpolate over the same duration as the servo pulse (85/90 ms automatic,
    // 300 ms manual) so OLED1 follows the actual command timing.
    bool revision_changed_big = false;
    int target_x = 0;
    int target_y = 0;
    uint32_t duration_ms = 0;
    uint32_t revision = 0;
    uint32_t start_tick = 0;
    if (servo_head_get_gaze(&target_x, &target_y, &duration_ms, &revision, &start_tick)) {
        if (revision != seen_gaze_revision) {
            transition_from_x = rendered_gaze_x;
            transition_from_y = rendered_gaze_y;
            transition_to_x = target_x;
            transition_to_y = target_y;
            transition_start_tick = start_tick;
            transition_duration_ms = duration_ms;
            seen_gaze_revision = revision;
            revision_changed_big =
                (abs(transition_to_x - transition_from_x) >= 2 ||
                 abs(transition_to_y - transition_from_y) >= 2);
        }

        const uint32_t elapsed_ticks =
            (uint32_t)xTaskGetTickCount() - transition_start_tick;
        const uint32_t duration_ticks = pdMS_TO_TICKS(transition_duration_ms);
        float t = duration_ticks == 0 ? 1.0f :
            (float)elapsed_ticks / (float)duration_ticks;
        if (t > 1.0f) t = 1.0f;
        // Smoothstep starts and stops gently, with the servo's duration as the timing reference.
        const float eased = t * t * (3.0f - 2.0f * t);
        const float interpolated_x = transition_from_x +
            (transition_to_x - transition_from_x) * eased;
        const float interpolated_y = transition_from_y +
            (transition_to_y - transition_from_y) * eased;
        // Round to the nearest pixel to reduce visible lag on short servo moves.
        rendered_gaze_x = (int)(interpolated_x >= 0.0f
            ? interpolated_x + 0.5f : interpolated_x - 0.5f);
        rendered_gaze_y = (int)(interpolated_y >= 0.0f
            ? interpolated_y + 0.5f : interpolated_y - 0.5f);
    }

    gaze_x = rendered_gaze_x;
    gaze_y = rendered_gaze_y;

    // Move both eye shapes and pupils together.
    eye_shift_x = gaze_x * 2;
    eye_shift_y = gaze_y * 2;

    // ---- "Hidup": kedip halus, mata bergetar kecil, napas, angguk ----
    // Semua ini hanya mengubah kelopak, pupil 1 piksel, dan posisi 1 piksel.
    // Arah pandang (gaze) tetap dari servo, jadi mata dan servo tetap selaras.
    const TickType_t now = xTaskGetTickCount();
    const bool sleeping = (step == 3);

    // Kelopak: target 100 saat tidur, 0 saat bangun. Menutup pelan, membuka lebih cepat.
    static float lid_base = 100.0f;
    const float lid_goal = sleeping ? 100.0f : 0.0f;
    if (lid_base < lid_goal)      lid_base = fminf(lid_goal, lid_base + 14.0f);
    else if (lid_base > lid_goal) lid_base = fmaxf(lid_goal, lid_base - 30.0f);

    // Kedip: bentuk sinus 0 -> 100 -> 0 selama ~210 ms, kadang kedip dobel.
    static TickType_t blink_start = 0;
    static TickType_t next_blink = 0;
    static bool blink_active = false;
    static bool double_pending = false;
    static face_state_t last_state = FACE_SLEEP;

    const face_state_t cur_state = face_get_state();
    if (cur_state != last_state) {
        last_state = cur_state;
        blink_active = false;
        double_pending = false;
        next_blink = now + pdMS_TO_TICKS(
            cur_state == FACE_SPEAKING ? 2600 + rnd(2600) : 1800 + rnd(3000));
    }
    if (next_blink == 0) next_blink = now + pdMS_TO_TICKS(2000 + rnd(2500));

    // Pandangan berpindah jauh -> orang biasanya berkedip.
    if (revision_changed_big && !blink_active && !sleeping && rnd(100) < 35) {
        blink_active = true;
        blink_start = now;
    }

    float blink_lid = 0.0f;
    if (!sleeping) {
        if (!blink_active && (int32_t)(now - next_blink) >= 0) {
            blink_active = true;
            blink_start = now;
        }
        if (blink_active) {
            const float t = (float)(now - blink_start) / (float)pdMS_TO_TICKS(210);
            if (t >= 1.0f) {
                blink_active = false;
                if (double_pending) {
                    double_pending = false;
                    next_blink = now + pdMS_TO_TICKS(120);   // kedip kedua cepat
                } else {
                    double_pending = (rnd(100) < 18);        // kadang kedip dobel
                    next_blink = now + pdMS_TO_TICKS(
                        (cur_state == FACE_SPEAKING ? 2800 : 1800) + rnd(3400));
                    if (double_pending) next_blink = now + pdMS_TO_TICKS(130);
                }
            } else {
                blink_lid = 100.0f * sinf(t * 3.14159265f);
            }
        }
    } else {
        blink_active = false;
    }

    int lid = (int)fmaxf(lid_base, blink_lid);
    if (sleeping && lid_base < 98.0f) {
        step = 0;               // sedang menutup mata: kelopak turun dulu, baru garis tidur
    }

    // Getar pupil: loncat kecil 1 piksel setiap 0.4 - 1.5 detik (hanya pupil).
    static int jit_x = 0, jit_y = 0;
    static TickType_t next_jit = 0;
    if ((int32_t)(now - next_jit) >= 0) {
        const int r = (int)rnd(10);
        jit_x = (r < 3) ? -1 : (r < 6 ? 1 : 0);
        jit_y = (rnd(10) < 2) ? (rnd(2) ? 1 : -1) : 0;
        next_jit = now + pdMS_TO_TICKS(400 + rnd(1100));
    }
    // Saat mata sedang bergerak mengikuti servo, jangan ditambah getar.
    const bool moving = (eye_shift_x != prev_eye_x) || (eye_shift_y != prev_eye_y);
    prev_eye_x = eye_shift_x;
    prev_eye_y = eye_shift_y;
    const int jx = (sleeping || moving) ? 0 : jit_x;
    const int jy = (sleeping || moving) ? 0 : jit_y;

    // Napas: seluruh wajah turun 1 piksel pelan-pelan (siklus ~4.4 detik).
    int bob = 0;
    if (!sleeping) {
        bob = (sinf((float)(now * portTICK_PERIOD_MS) * (6.2831853f / 4400.0f)) > 0.35f) ? 1 : 0;
    }
    // Angguk kecil saat suara keras (dengan histeresis supaya tidak berkedip-kedip).
    static bool loud = false;
    if (expr == 2 && !sleeping) {
        const int lv = face_get_speech_level();
        if (!loud && lv > 150) loud = true;
        else if (loud && lv < 90) loud = false;
    } else {
        loud = false;
    }
    sy += bob + (loud ? 1 : 0);

    display_render_mochi_lid(
        expr, step, sx, sy,
        gaze_x, gaze_y,
        eye_shift_x, eye_shift_y,
        lid, jx, jy
    );
}

// Keep rendering the current face during THINKING/SPEAKING holds so the
// OLED marquee advances continuously instead of freezing during vTaskDelay().
static bool hold_render(face_state_t state,
                        int expr,
                        int gaze_x,
                        int gaze_y,
                        uint32_t duration_ms)
{
    if (!state_ok(state)) return false;

    const TickType_t total_ticks = pdMS_TO_TICKS(duration_ms);
    const TickType_t start_tick = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start_tick) < total_ticks) {
        if (!state_ok(state)) return false;

        render(expr, 0, 0, 0, gaze_x, gaze_y);
        vTaskDelay(FRAME_DELAY);
    }

    return state_ok(state);
}

static void thinking_sequence(void)
{
    if (!state_ok(FACE_THINKING)) return;

    // Thinking expressions remain, but gaze is no longer generated independently.
    // Every render follows the shared direction published by the servo controller.
    static const int moods[] = {10, 11, 12, 13, 14, 15};

    // Slow, slightly varied holds: each mood stays readable for 5–8 seconds.
    static const uint16_t min_hold_ms[] = {5000, 5400, 5200, 5000, 5600, 5300};
    static const uint16_t max_hold_ms[] = {7600, 8000, 7400, 7800, 8000, 7600};

    for (unsigned mood = 0; mood < sizeof(moods) / sizeof(moods[0]); ++mood) {
        if (!state_ok(FACE_THINKING)) return;
        render(moods[mood], 0, 0, 0, 0, 0);

        const TickType_t start = xTaskGetTickCount();
        const uint32_t hold_ms = min_hold_ms[mood] +
            rnd(max_hold_ms[mood] - min_hold_ms[mood] + 1);
        const TickType_t duration = pdMS_TO_TICKS(hold_ms);
        while ((xTaskGetTickCount() - start) < duration) {
            if (!state_ok(FACE_THINKING)) return;
            render(moods[mood], 0, 0, 0, 0, 0);
            vTaskDelay(FRAME_DELAY);
        }
    }
}

static void speaking_sequence(void)
{
    // Gaze is shared with the servo controller; blink frames are non-blocking.
    if (!state_ok(FACE_SPEAKING)) return;

    render(2, 0, 0, 0, 0, 0);
    vTaskDelay(FRAME_DELAY);
}

static void sleep_sequence(void)
{
    if (!state_ok(FACE_SLEEP)) return;

    // Sleep reuses the same face anchors with closed eyes, sleep mouth and Z overlay.
    // Keep the sleepy Zs moving and add a barely visible one-pixel breathing
    // motion; a single render followed by a long delay made the face look frozen.
    const TickType_t start = xTaskGetTickCount();
    const TickType_t duration = pdMS_TO_TICKS(1200);
    uint32_t frame = 0;
    while (state_ok(FACE_SLEEP) &&
           (xTaskGetTickCount() - start) < duration) {
        const int breath_y = ((frame / 7U) % 2U) ? 1 : 0;
        render(0, 3, 0, breath_y, 0, 0);
        ++frame;
        vTaskDelay(FRAME_DELAY);
    }
}

static void emotion_sequence(face_state_t state, int expr)
{
    while (state_ok(state)) { render(expr, 0, 0, 0, 0, 0); vTaskDelay(FRAME_DELAY); }
}

void face_animation_run_state(face_state_t state)
{
    switch (state) {
        case FACE_THINKING: thinking_sequence(); break;
        case FACE_SPEAKING: speaking_sequence(); break;
        case FACE_SLEEP:    sleep_sequence(); break;
        case FACE_HAPPY:    emotion_sequence(FACE_HAPPY, 16); break;
        case FACE_ANGRY:    emotion_sequence(FACE_ANGRY, 17); break;
        default:            sleep_sequence(); break;
    }
}


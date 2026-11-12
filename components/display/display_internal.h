#pragma once

#include "display.h"

// Private framebuffer bridge: display.cpp owns the OLED driver and buffer.
uint8_t *display_framebuffer(void);
void display_clear_framebuffer(void);

// Called only by the controller task. Keeps movement sequences independent
// from task lifecycle and state override management.
void face_animation_run_state(face_state_t state);

// Thin layer between Dust and the ESP-Mosaico hardware. Everything board-specific lives in
// board_port.c, so the rest of the firmware never touches a driver directly.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t board_port_init(void);

// Push a full 480x480 RGB565 frame to the panel.
void board_port_flush(const uint16_t *fb);

// Single touch point in panel pixels. Returns false when not touched.
bool board_port_touch(int *x, int *y);

// Acceleration in g (x, y, z). Returns false if the IMU isn't wired up yet;
// lifting then falls back to a long press on the screen.
bool board_port_accel(float g[3]);

// Strength of the magnetic field at the screen, in microtesla (the larger of the two BMM150
// readings). Returns false if the magnetometers aren't wired up yet.
bool board_port_mag(float *ut);

// Both magnetometers' full field vectors, in µT, in the tile's own frame (x right, y up,
// z out of the screen). Returns false if they aren't wired up yet.
bool board_port_mag3(float v[2][3]);

// Rotate the panel (and touch) by quarter turns clockwise, for tiles mounted turned.
void board_port_set_rotation(int quarter_turns);

// Vibration motor (GPIO 8) on or off: Dust's Buzz.
void board_port_buzz(bool on);

// Charge (%) and temperature (°C) from the BQ27220 fuel gauge. False if it isn't answering.
bool board_port_battery(int *percent, float *temp_c);

// The AI button (GPIO 7): true while it's held down.
bool board_port_ai_button(void);

// Which sensors answered at boot (for the health report).
bool board_port_has_imu(void);
bool board_port_has_mag(void);

// Memory for frame and render buffers (PSRAM).
void *board_port_alloc(size_t bytes);

// True if the panel wants big-endian RGB565 (most QSPI AMOLED controllers do).
bool board_port_swap_bytes(void);

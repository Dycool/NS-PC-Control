#pragma once

#include <cstdint>

void mouse_input_start(void* hwnd);
void mouse_input_stop();

void mouse_input_reset();

void mouse_apply_right_stick(uint8_t& rx, uint8_t& ry);

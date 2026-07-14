#pragma once

#include <cstdint>

void mouse_input_start(void* hwnd);
void mouse_input_stop();

void mouse_input_reset();
bool mouse_input_native_joycon_supported();

void mouse_apply_right_stick(uint8_t& rx, uint8_t& ry);
void mouse_consume_joycon_input(int32_t& dx, int32_t& dy, int32_t& scroll_y);
void mouse_joycon_button_state(bool& left, bool& right);

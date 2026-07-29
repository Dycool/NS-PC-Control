#pragma once

#include <cstdint>
#include <string>

void mouse_input_start(void* hwnd);
void mouse_input_stop();

void mouse_input_reset();
bool mouse_input_native_joycon_supported();
void mouse_input_add_focused_motion(int32_t dx, int32_t dy);
void mouse_input_add_focused_scroll(int32_t delta);

void mouse_apply_right_stick(uint8_t& rx, uint8_t& ry);
void mouse_consume_joycon_input(int32_t& dx, int32_t& dy, int32_t& scroll_y);
void mouse_joycon_button_state(bool& left, bool& right);
// Returns true when the platform has an authoritative global state for this
// normalized key/button name. `down` is valid even when it is false.
bool mouse_input_query_key_state(const std::string& name, bool& down);

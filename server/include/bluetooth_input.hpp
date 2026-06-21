#pragma once

#include <stop_token>

bool bluetooth_input_available();
void bluetooth_input_thread(std::stop_token stoken);

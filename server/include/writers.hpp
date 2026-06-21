#pragma once

#include <stop_token>

void legacy_writer_thread(std::stop_token stoken, int hz);
void writer_thread(std::stop_token stoken, int hz);
void stats_thread(std::stop_token stoken);

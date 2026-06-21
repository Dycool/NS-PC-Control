#pragma once

#include <atomic>
#include <string>
#include <vector>

extern std::atomic<bool> g_cliRunning;

void print_cli_usage(const char* exe);
int cli_main(const std::vector<std::string>& original_args);

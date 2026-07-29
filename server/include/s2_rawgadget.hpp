#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <span>
#include <vector>

bool s2_rawgadget_module_available();
bool s2_rawgadget_setup(bool force, const char* reason);
void s2_rawgadget_teardown();

bool s2_rawgadget_nodes_ready();
bool s2_rawgadget_transport_active();
bool s2_rawgadget_io_ready();
bool s2_rawgadget_host_enabled();

bool s2_rawgadget_submit_input_report(const uint8_t* data, size_t len);
bool s2_rawgadget_poll_output_report(std::vector<uint8_t>& out_report);
void s2_rawgadget_drain_output();
bool s2_rawgadget_poll_vendor_report(std::vector<uint8_t>& out_report);
bool s2_rawgadget_submit_vendor_report(const uint8_t* data, size_t len,
                                       std::span<const uint8_t> request);

bool s2_rawgadget_pop_console_audio(std::span<uint8_t> frame,
                                    std::chrono::milliseconds timeout);
bool s2_rawgadget_queue_microphone_audio(std::span<const uint8_t> data);
void s2_rawgadget_get_playback_control(bool& muted, int16_t& volume_1_256db);
void s2_rawgadget_get_capture_control(bool& muted, int16_t& volume_1_256db);

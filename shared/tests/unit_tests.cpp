#include "shared/protocol.hpp"
#include "shared/macros.hpp"
#include "shared/sha256.h"
#include "shared/sdl_input.hpp"
#include "app_state.hpp"
#include "web_server.hpp"
#include "bluetooth_input.hpp"

#include <iostream>
#include <cassert>
#include <cctype>
#include <string>
#include <vector>
#include <span>
#include <cstring>
#include <thread>
#include <chrono>
#include <ranges>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <format>

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n"; \
            std::exit(1); \
        } \
    } while (0)

#define ASSERT_EQ(val1, val2) \
    do { \
        if ((val1) != (val2)) { \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " \
                      << #val1 << " == " << #val2 << " (" << (val1) << " != " << (val2) << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

void test_crypto() {
    std::cout << "[test] Running cryptography checks...\n";

    uint8_t key1[32];
    uint8_t key2[32];
    derive_key("my_super_secret_test_key", key1);
    derive_key("my_super_secret_test_key", key2);

    for (int i = 0; i < 32; ++i) {
        ASSERT_EQ(key1[i], key2[i]);
    }

    uint8_t key_diff[32];
    derive_key("another_key", key_diff);
    bool distinct = false;
    for (int i = 0; i < 32; ++i) {
        if (key1[i] != key_diff[i]) distinct = true;
    }
    ASSERT_TRUE(distinct);

    uint8_t msg[] = "Hello World! This is a test input payload.";
    uint8_t out[32];
    hmac_sha256(std::span<const uint8_t>(key1, 32), std::span<const uint8_t>(msg, sizeof(msg)), std::span<uint8_t, 32>(out, 32));

    int verify_ok = hmac_verify(std::span<const uint8_t>(key1, 32), std::span<const uint8_t>(msg, sizeof(msg)), std::span<const uint8_t>(out, 32));
    ASSERT_EQ(verify_ok, 0);

    msg[0] = 'h';
    int verify_bad = hmac_verify(std::span<const uint8_t>(key1, 32), std::span<const uint8_t>(msg, sizeof(msg)), std::span<const uint8_t>(out, 32));
    ASSERT_TRUE(verify_bad != 0);

    msg[0] = 'H';
    int verify_bad_key = hmac_verify(std::span<const uint8_t>(key_diff, 32), std::span<const uint8_t>(msg, sizeof(msg)), std::span<const uint8_t>(out, 32));
    ASSERT_TRUE(verify_bad_key != 0);

    std::cout << "[test] Cryptography checks passed.\n";
}

void test_protocol_sizes() {
    std::cout << "[test] Running wire-format and size verification checks...\n";

    ASSERT_EQ(sizeof(ns::HIDReport), 8);
    ASSERT_EQ(sizeof(ns::MultiReport), 32);
    ASSERT_EQ(sizeof(ns::MotionReport), 12);
    ASSERT_EQ(sizeof(ns::ExtendedHIDReport), 24);
    ASSERT_EQ(sizeof(ns::ExtendedMultiReport), 96);
    ASSERT_EQ(sizeof(ns::ExtendedHIDReport3), 48);
    ASSERT_EQ(sizeof(ns::ExtendedMultiReport3), 192);
    ASSERT_EQ(sizeof(ns::Packet), 68);
    ASSERT_EQ(sizeof(ns::RumblePacket), 8);
    ASSERT_EQ(sizeof(ns::PrecisionRumblePacket), 20);
    ASSERT_EQ(sizeof(ns::ServerInfoProbe), 8);
    ASSERT_EQ(sizeof(ns::ServerInfoReply), 16);

    std::cout << "[test] Wire-format and size verification passed.\n";
}

void test_macro_parsing() {
    std::cout << "[test] Running macro parser and validation checks...\n";

    ASSERT_EQ(ns::macro::trim("  hello world  "), "hello world");
    ASSERT_EQ(ns::macro::trim("\n\t  abc  \r\n"), "abc");
    ASSERT_EQ(ns::macro::upper("aBcDef123!"), "ABCDEF123!");

    uint32_t val = 0;
    ASSERT_TRUE(ns::macro::parse_uint32_strict("123456", val));
    ASSERT_EQ(val, 123456);
    ASSERT_TRUE(ns::macro::parse_uint32_strict("0", val));
    ASSERT_EQ(val, 0);
    ASSERT_TRUE(!ns::macro::parse_uint32_strict("123a", val));
    ASSERT_TRUE(!ns::macro::parse_uint32_strict("-123", val));
    ASSERT_TRUE(!ns::macro::parse_uint32_strict("", val));
    ASSERT_TRUE(!ns::macro::parse_uint32_strict("4294967296", val));

    ASSERT_EQ(ns::macro::button_bit("A"), ns::BTN_A);
    ASSERT_EQ(ns::macro::button_bit("BTN_A"), ns::BTN_A);
    ASSERT_EQ(ns::macro::button_bit("X"), ns::BTN_X);
    ASSERT_EQ(ns::macro::button_bit("ZL"), ns::BTN_ZL);
    ASSERT_EQ(ns::macro::button_bit("MINUS"), ns::BTN_MINUS);
    ASSERT_EQ(ns::macro::button_bit("invalid_button"), 0);

    auto res_raw = ns::macro::extract_commands_text("WAIT 100; A 200");
    ASSERT_TRUE(res_raw.has_value());
    ASSERT_EQ(res_raw.value(), "WAIT 100; A 200");

    auto res_obj = ns::macro::extract_commands_text("{\"name\":\"test_macro\",\"commands\":\"A 10; B 20\"}");
    ASSERT_TRUE(res_obj.has_value());
    ASSERT_EQ(res_obj.value(), "A 10; B 20");

    auto res_arr = ns::macro::extract_commands_text("[\"A 10\", \"B 20\"]");
    ASSERT_TRUE(res_arr.has_value());
    ASSERT_EQ(res_arr.value(), "A 10;B 20");

    auto res_bad = ns::macro::extract_commands_text("{invalid_json}");
    ASSERT_TRUE(!res_bad.has_value());

    std::string test_script = "WAIT 100\nA 250\nR+LSTICK_LEFT 400\nLOOP 2";
    auto parsed_steps = ns::macro::parse_text(test_script);
    ASSERT_TRUE(!parsed_steps.empty());

    ASSERT_EQ(parsed_steps[0].duration_ms, 100);
    ASSERT_EQ(parsed_steps[0].buttons, 0);

    ASSERT_EQ(parsed_steps[1].duration_ms, 250);
    ASSERT_EQ(parsed_steps[1].buttons, ns::BTN_A);

    ASSERT_EQ(parsed_steps[2].duration_ms, 400);
    ASSERT_EQ(parsed_steps[2].buttons, ns::BTN_R);
    ASSERT_TRUE(parsed_steps[2].has_lstick);
    ASSERT_EQ(parsed_steps[2].lx, 0);
    ASSERT_EQ(parsed_steps[2].ly, 128);

    std::string pretty_out, err_out;
    bool pretty_ok = ns::macro::validate_to_pretty_json("A 100\nB 200", pretty_out, err_out, "Test Name");
    ASSERT_TRUE(pretty_ok);
    ASSERT_TRUE(pretty_out.find("\"name\": \"Test Name\"") != std::string::npos);
    ASSERT_TRUE(pretty_out.find("\"commands\"") != std::string::npos);

    std::cout << "[test] Macro parser and validation checks passed.\n";
}

void test_macro_recorder_runtime() {
    std::cout << "[test] Running macro recorder and playback checks...\n";

    ns::macro::Recorder rec;
    rec.start(1000);
    
    ns::HIDReport r1;
    r1.buttons = ns::BTN_A;
    rec.sample(r1, 1000);
    
    ns::HIDReport r2;
    r2.buttons = ns::BTN_B;
    rec.sample(r2, 100000);
    
    std::string macro_result = rec.stop(250000);
    ASSERT_TRUE(!macro_result.empty());
    
    ns::macro::Runtime rt;
    bool started = ns::macro::runtime_start_text(rt, "A 100\nB 150", 1000);
    ASSERT_TRUE(started);
    ASSERT_TRUE(ns::macro::runtime_running(rt, 1000));
    
    ns::HIDReport out_rep;
    bool playing = ns::macro::runtime_report(rt, 1000 + 50000, out_rep);
    ASSERT_TRUE(playing);
    ASSERT_EQ(out_rep.buttons, ns::BTN_A);
    
    playing = ns::macro::runtime_report(rt, 1000 + 150000, out_rep);
    ASSERT_TRUE(playing);
    ASSERT_EQ(out_rep.buttons, ns::BTN_B);

    playing = ns::macro::runtime_report(rt, 1000 + 350000, out_rep);
    ASSERT_TRUE(!playing);

    std::cout << "[test] Macro recorder and playback checks passed.\n";
}

void test_sdl_input_helpers() {
    std::cout << "[test] Running Bluetooth controller and SDL3 input mapping checks...\n";

    ASSERT_EQ(sdl_axis_to_byte(0), 128);
    ASSERT_EQ(sdl_axis_to_byte(5000), 128);
    ASSERT_EQ(sdl_axis_to_byte(-5000), 128);
    ASSERT_EQ(sdl_axis_to_byte(-32768), 0);
    ASSERT_EQ(sdl_axis_to_byte(32767), 255);
    ASSERT_EQ(sdl_axis_to_byte(20000), 189);

    DigitalReleaseFilter filter;
    filter.reset();

    ns::HIDReport report;
    report.buttons = ns::BTN_A;
    
    filter.apply(report, 0);
    ASSERT_EQ(report.buttons, ns::BTN_A);

    report.buttons = 0;
    filter.apply(report, 10000);
    ASSERT_EQ(report.buttons, ns::BTN_A);

    report.buttons = 0;
    filter.apply(report, 50000);
    ASSERT_EQ(report.buttons, 0);

    std::cout << "[test] Bluetooth controller and SDL3 input mapping checks passed.\n";
}

void test_protocol_helpers() {
    std::cout << "[test] Running protocol helper function checks...\n";

    ns::Packet good_pkt;
    good_pkt.magic = ns::PROTO_MAGIC;
    good_pkt.version = ns::PROTO_VERSION;
    ASSERT_TRUE(ns::packet_ok(good_pkt));

    ns::Packet bad_magic;
    bad_magic.magic = 0xDEADBEEF;
    bad_magic.version = ns::PROTO_VERSION;
    ASSERT_TRUE(!ns::packet_ok(bad_magic));

    ns::Packet bad_version;
    bad_version.magic = ns::PROTO_MAGIC;
    bad_version.version = 99;
    ASSERT_TRUE(!ns::packet_ok(bad_version));

    ns::HIDReport neutral;
    neutral.reset();
    ASSERT_TRUE(input_is_neutral(neutral));

    ns::HIDReport not_neutral = neutral;
    not_neutral.buttons = ns::BTN_A;
    ASSERT_TRUE(!input_is_neutral(not_neutral));

    ns::HIDReport stick_moved = neutral;
    stick_moved.lx = 200;
    ASSERT_TRUE(!input_is_neutral(stick_moved));

    ns::HIDReport hat_active = neutral;
    hat_active.hat = ns::HAT_N;
    ASSERT_TRUE(!input_is_neutral(hat_active));

    ns::MotionReport mot_neutral;
    mot_neutral.reset();
    ASSERT_TRUE(motion_is_neutral(mot_neutral));

    ns::MotionReport mot_active;
    mot_active.reset();
    mot_active.ax = 500;
    ASSERT_TRUE(!motion_is_neutral(mot_active));

    ns::MotionReport mot_edge;
    mot_edge.reset();
    mot_edge.gx = 63;
    ASSERT_TRUE(motion_is_neutral(mot_edge));

    ns::MotionReport mot_over;
    mot_over.reset();
    mot_over.gx = 64;
    ASSERT_TRUE(!motion_is_neutral(mot_over));

    ns::ExtendedHIDReport ext_neutral;
    ext_neutral.reset();
    ASSERT_TRUE(extended_is_neutral(ext_neutral));

    ns::ExtendedHIDReport ext_btn;
    ext_btn.reset();
    ext_btn.input.buttons = ns::BTN_X;
    ASSERT_TRUE(!extended_is_neutral(ext_btn));

    ns::ExtendedHIDReport ext_motion;
    ext_motion.reset();
    ext_motion.has_motion = 1;
    ext_motion.motion.ax = 500;
    ASSERT_TRUE(!extended_is_neutral(ext_motion));

    ns::ExtendedHIDReport ext_motion_neutral;
    ext_motion_neutral.reset();
    ext_motion_neutral.has_motion = 1;
    ext_motion_neutral.motion.reset();
    ASSERT_TRUE(extended_is_neutral(ext_motion_neutral));

    ns::MultiReport legacy;
    legacy.reset();
    legacy.p1.buttons = ns::BTN_A;
    legacy.p1.lx = 200;
    legacy.p2.buttons = ns::BTN_B;
    ns::ExtendedMultiReport ext_out;
    legacy_multi_to_extended(legacy, ext_out);
    ASSERT_EQ(ext_out.p1.input.buttons, ns::BTN_A);
    ASSERT_EQ(ext_out.p1.input.lx, 200);
    ASSERT_EQ(ext_out.p2.input.buttons, ns::BTN_B);
    ASSERT_EQ(ext_out.p1.has_motion, 0);
    ASSERT_EQ(ext_out.p3.input.buttons, 0);

    ns::ExtendedHIDReport3 ext3;
    ext3.reset();
    ext3.input.buttons = ns::BTN_ZL;
    ext3.has_motion = 1;
    ext3.motion[0].ax = 100;
    ext3.motion[1].ax = 200;
    ext3.motion[2].ax = 300;
    ns::ExtendedHIDReport ext1_out;
    extended3_to_extended_latest(ext3, ext1_out);
    ASSERT_EQ(ext1_out.input.buttons, ns::BTN_ZL);
    ASSERT_EQ(ext1_out.has_motion, 1);
    ASSERT_EQ(ext1_out.motion.ax, 300);

    std::cout << "[test] Protocol helper function checks passed.\n";
}

void test_all_button_tokens() {
    std::cout << "[test] Running complete button/stick/dpad token checks...\n";

    ASSERT_EQ(ns::macro::button_bit("Y"), ns::BTN_Y);
    ASSERT_EQ(ns::macro::button_bit("B"), ns::BTN_B);
    ASSERT_EQ(ns::macro::button_bit("A"), ns::BTN_A);
    ASSERT_EQ(ns::macro::button_bit("X"), ns::BTN_X);
    ASSERT_EQ(ns::macro::button_bit("L"), ns::BTN_L);
    ASSERT_EQ(ns::macro::button_bit("R"), ns::BTN_R);
    ASSERT_EQ(ns::macro::button_bit("ZL"), ns::BTN_ZL);
    ASSERT_EQ(ns::macro::button_bit("ZR"), ns::BTN_ZR);
    ASSERT_EQ(ns::macro::button_bit("MINUS"), ns::BTN_MINUS);
    ASSERT_EQ(ns::macro::button_bit("PLUS"), ns::BTN_PLUS);
    ASSERT_EQ(ns::macro::button_bit("LSTICK"), ns::BTN_LSTICK);
    ASSERT_EQ(ns::macro::button_bit("RSTICK"), ns::BTN_RSTICK);
    ASSERT_EQ(ns::macro::button_bit("HOME"), ns::BTN_HOME);
    ASSERT_EQ(ns::macro::button_bit("CAPTURE"), ns::BTN_CAPTURE);

    ASSERT_EQ(ns::macro::button_bit("BTN_Y"), ns::BTN_Y);
    ASSERT_EQ(ns::macro::button_bit("BTN_B"), ns::BTN_B);
    ASSERT_EQ(ns::macro::button_bit("BTN_A"), ns::BTN_A);
    ASSERT_EQ(ns::macro::button_bit("BTN_X"), ns::BTN_X);
    ASSERT_EQ(ns::macro::button_bit("BTN_L"), ns::BTN_L);
    ASSERT_EQ(ns::macro::button_bit("BTN_R"), ns::BTN_R);
    ASSERT_EQ(ns::macro::button_bit("BTN_ZL"), ns::BTN_ZL);
    ASSERT_EQ(ns::macro::button_bit("BTN_ZR"), ns::BTN_ZR);
    ASSERT_EQ(ns::macro::button_bit("BTN_MINUS"), ns::BTN_MINUS);
    ASSERT_EQ(ns::macro::button_bit("BTN_PLUS"), ns::BTN_PLUS);
    ASSERT_EQ(ns::macro::button_bit("BTN_LSTICK"), ns::BTN_LSTICK);
    ASSERT_EQ(ns::macro::button_bit("BTN_RSTICK"), ns::BTN_RSTICK);
    ASSERT_EQ(ns::macro::button_bit("BTN_HOME"), ns::BTN_HOME);
    ASSERT_EQ(ns::macro::button_bit("BTN_CAPTURE"), ns::BTN_CAPTURE);

    ASSERT_EQ(ns::macro::button_bit("INVALID"), 0);
    ASSERT_EQ(ns::macro::button_bit(""), 0);
    ASSERT_EQ(ns::macro::button_bit("BTN_INVALID"), 0);

    auto parse_dpad = [](const std::string& cmd) -> uint8_t {
        auto steps = ns::macro::parse_text(cmd + " 100");
        if (steps.empty()) return ns::HAT_NEUTRAL;
        return steps[0].hat;
    };
    ASSERT_EQ(parse_dpad("DPAD_UP"), ns::HAT_N);
    ASSERT_EQ(parse_dpad("DPAD_DOWN"), ns::HAT_S);
    ASSERT_EQ(parse_dpad("DPAD_LEFT"), ns::HAT_W);
    ASSERT_EQ(parse_dpad("DPAD_RIGHT"), ns::HAT_E);

    auto parse_stick_lx = [](const std::string& cmd) -> uint8_t {
        auto steps = ns::macro::parse_text(cmd + " 100");
        if (steps.empty()) return 128;
        return steps[0].lx;
    };
    auto parse_stick_ly = [](const std::string& cmd) -> uint8_t {
        auto steps = ns::macro::parse_text(cmd + " 100");
        if (steps.empty()) return 128;
        return steps[0].ly;
    };
    auto parse_stick_rx = [](const std::string& cmd) -> uint8_t {
        auto steps = ns::macro::parse_text(cmd + " 100");
        if (steps.empty()) return 128;
        return steps[0].rx;
    };
    auto parse_stick_ry = [](const std::string& cmd) -> uint8_t {
        auto steps = ns::macro::parse_text(cmd + " 100");
        if (steps.empty()) return 128;
        return steps[0].ry;
    };

    ASSERT_EQ(parse_stick_lx("LSTICK_LEFT"), 0);
    ASSERT_EQ(parse_stick_ly("LSTICK_LEFT"), 128);
    ASSERT_EQ(parse_stick_lx("LSTICK_RIGHT"), 255);
    ASSERT_EQ(parse_stick_ly("LSTICK_RIGHT"), 128);
    ASSERT_EQ(parse_stick_lx("LSTICK_UP"), 128);
    ASSERT_EQ(parse_stick_ly("LSTICK_UP"), 0);
    ASSERT_EQ(parse_stick_lx("LSTICK_DOWN"), 128);
    ASSERT_EQ(parse_stick_ly("LSTICK_DOWN"), 255);

    ASSERT_EQ(parse_stick_rx("RSTICK_LEFT"), 0);
    ASSERT_EQ(parse_stick_ry("RSTICK_LEFT"), 128);
    ASSERT_EQ(parse_stick_rx("RSTICK_RIGHT"), 255);
    ASSERT_EQ(parse_stick_ry("RSTICK_RIGHT"), 128);
    ASSERT_EQ(parse_stick_rx("RSTICK_UP"), 128);
    ASSERT_EQ(parse_stick_ry("RSTICK_UP"), 0);
    ASSERT_EQ(parse_stick_rx("RSTICK_DOWN"), 128);
    ASSERT_EQ(parse_stick_ry("RSTICK_DOWN"), 255);

    std::cout << "[test] Complete button/stick/dpad token checks passed.\n";
}

void test_macro_edge_cases() {
    std::cout << "[test] Running macro edge case checks...\n";

    auto combo = ns::macro::parse_text("A+B+X 100");
    ASSERT_TRUE(!combo.empty());
    ASSERT_EQ(combo[0].buttons, (uint16_t)(ns::BTN_A | ns::BTN_B | ns::BTN_X));
    ASSERT_EQ(combo[0].duration_ms, 100);

    auto dpad_combo = ns::macro::parse_text("DPAD_UP+A 200");
    ASSERT_TRUE(!dpad_combo.empty());
    ASSERT_EQ(dpad_combo[0].buttons, ns::BTN_A);
    ASSERT_EQ(dpad_combo[0].hat, ns::HAT_N);

    auto loop_steps = ns::macro::parse_text("A 50\nB 50\nLOOP 3");
    ASSERT_TRUE(!loop_steps.empty());
    ASSERT_EQ(loop_steps.size(), 6u);

    uint64_t total = ns::macro::total_ms(loop_steps);
    ASSERT_EQ(total, 300u);

    ns::macro::Step found;
    bool ok = ns::macro::step_at(loop_steps, 0, found);
    ASSERT_TRUE(ok);
    ASSERT_EQ(found.buttons, ns::BTN_A);

    ok = ns::macro::step_at(loop_steps, 50, found);
    ASSERT_TRUE(ok);
    ASSERT_EQ(found.buttons, ns::BTN_B);

    ok = ns::macro::step_at(loop_steps, 100, found);
    ASSERT_TRUE(ok);
    ASSERT_EQ(found.buttons, ns::BTN_A);

    ok = ns::macro::step_at(loop_steps, 999, found);
    ASSERT_TRUE(!ok);

    ns::HIDReport rep;
    bool rep_ok = ns::macro::report_at(loop_steps, 25, rep);
    ASSERT_TRUE(rep_ok);
    ASSERT_EQ(rep.buttons, ns::BTN_A);

    auto empty = ns::macro::parse_text("");
    ASSERT_TRUE(empty.empty());

    std::vector<ns::macro::Step> out_steps;
    bool valid = ns::macro::validate_text("FAKE_BUTTON 100", out_steps);
    ASSERT_TRUE(!valid);

    auto extracted = ns::macro::extract_commands_text("[\"A 100\", \"B 200\"]");
    ASSERT_TRUE(extracted.has_value());
    auto arr_parsed = ns::macro::parse_text(extracted.value());
    ASSERT_TRUE(!arr_parsed.empty());
    ASSERT_EQ(arr_parsed[0].buttons, ns::BTN_A);

    std::cout << "[test] Macro edge case checks passed.\n";
}

void test_macro_entry_system() {
    std::cout << "[test] Running macro entry serialization checks...\n";

    ns::macro::Entry entry1;
    entry1.name = "Test Macro";
    entry1.hotkey = "";
    entry1.json = "A 100\nB 200";
    std::string json_out = ns::macro::entry_to_object_json(entry1);
    ASSERT_TRUE(json_out.find("\"name\"") != std::string::npos);
    ASSERT_TRUE(json_out.find("Test Macro") != std::string::npos);
    ASSERT_TRUE(json_out.find("\"commands\"") != std::string::npos);

    std::vector<ns::macro::Entry> entries;
    ns::macro::Entry e1{.name = "Macro1", .hotkey = "", .json = "A 100"};
    ns::macro::Entry e2{.name = "Macro2", .hotkey = "", .json = "B 200"};
    entries.push_back(e1);
    entries.push_back(e2);
    std::string multi_json = ns::macro::entries_to_json(entries);
    ASSERT_TRUE(multi_json.find("Macro1") != std::string::npos);
    ASSERT_TRUE(multi_json.find("Macro2") != std::string::npos);

    std::string name_from_json = ns::macro::extract_name_or_default(
        "{\"name\":\"My Custom Name\",\"commands\":\"A 100\"}", "Fallback");
    ASSERT_EQ(name_from_json, "My Custom Name");

    std::string name_fallback = ns::macro::extract_name_or_default("A 100; B 200", "Fallback");
    ASSERT_EQ(name_fallback, "Fallback");

    std::string btn_text = ns::macro::buttons_to_text(ns::BTN_A | ns::BTN_B);
    ASSERT_TRUE(btn_text.find("A") != std::string::npos);
    ASSERT_TRUE(btn_text.find("B") != std::string::npos);

    ns::HIDReport rec_rep;
    rec_rep.reset();
    rec_rep.buttons = ns::BTN_X;
    rec_rep.lx = 0;
    auto frame = ns::macro::record_frame_from_report(rec_rep);
    ASSERT_EQ(frame.buttons, ns::BTN_X);

    std::cout << "[test] Macro entry serialization checks passed.\n";
}

void test_sdl_input_extended() {
    std::cout << "[test] Running extended SDL3 input pipeline checks...\n";

    ASSERT_EQ(sdl_axis_to_byte(32767, true), 0);
    ASSERT_EQ(sdl_axis_to_byte(-32768, true), 255);
    ASSERT_EQ(sdl_axis_to_byte(0, true), 128);

    ASSERT_EQ(sdl_axis_to_byte(3000, false, 5000), 128);
    ASSERT_EQ(sdl_axis_to_byte(6000, false, 5000), 132);
    ASSERT_EQ(sdl_axis_to_byte(0, false, 0), 128);

    ASSERT_EQ(clamp_motion_i16(40000.0f), 32767);
    ASSERT_EQ(clamp_motion_i16(-40000.0f), -32768);
    ASSERT_EQ(clamp_motion_i16(0.0f), 0);
    ASSERT_EQ(clamp_motion_i16(100.5f), 101);

    ASSERT_EQ(gyro_deadzone_i16(0), 0);
    ASSERT_EQ(gyro_deadzone_i16(1000), 1000);

    DigitalReleaseFilter hat_filter;
    hat_filter.reset();

    ns::HIDReport hat_rep;
    hat_rep.reset();
    hat_rep.hat = ns::HAT_N;

    hat_filter.apply(hat_rep, 0);
    ASSERT_EQ(hat_rep.hat, ns::HAT_N);

    hat_rep.hat = ns::HAT_NEUTRAL;
    hat_filter.apply(hat_rep, 10000);
    ASSERT_EQ(hat_rep.hat, ns::HAT_N);

    hat_rep.hat = ns::HAT_NEUTRAL;
    hat_filter.apply(hat_rep, 50000);
    ASSERT_EQ(hat_rep.hat, ns::HAT_NEUTRAL);

    DigitalReleaseFilter multi_filter;
    multi_filter.reset();

    ns::HIDReport multi_rep;
    multi_rep.reset();
    multi_rep.buttons = ns::BTN_A | ns::BTN_B;

    multi_filter.apply(multi_rep, 0);
    ASSERT_EQ(multi_rep.buttons, (uint16_t)(ns::BTN_A | ns::BTN_B));

    multi_rep.buttons = ns::BTN_B;
    multi_filter.apply(multi_rep, 10000);
    ASSERT_EQ(multi_rep.buttons, (uint16_t)(ns::BTN_A | ns::BTN_B));

    multi_rep.buttons = ns::BTN_B;
    multi_filter.apply(multi_rep, 50000);
    ASSERT_EQ(multi_rep.buttons, ns::BTN_B);

    std::cout << "[test] Extended SDL3 input pipeline checks passed.\n";
}

static void reset_server_state() {
    g_ctx.running.store(true, std::memory_order_relaxed);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        g_ctx.clients[i] = ClientSession{};
    }
    memset(g_ctx.rate_table, 0, sizeof(g_ctx.rate_table));
    g_ctx.pkts_rx.store(0);
    g_ctx.hid_writes.store(0);
    g_ctx.switch2_usb_host_connected.store(false);
    g_ctx.switch2_last_usb_activity_us.store(0);
    g_ctx.switch2_force_next_wake.store(false);
    g_ctx.switch2_suspend_disconnect_seq.store(0);
}

void test_client_session_management() {
    std::cout << "[test] Running client session management checks...\n";
    reset_server_state();

    uint64_t now = ns::now_us();

    sockaddr_in addr1{}, addr2{}, addr3{}, addr4{}, addr5{};
    addr1.sin_family = AF_INET; addr1.sin_addr.s_addr = htonl(0x01010101); addr1.sin_port = htons(10001);
    addr2.sin_family = AF_INET; addr2.sin_addr.s_addr = htonl(0x02020202); addr2.sin_port = htons(10002);
    addr3.sin_family = AF_INET; addr3.sin_addr.s_addr = htonl(0x03030303); addr3.sin_port = htons(10003);
    addr4.sin_family = AF_INET; addr4.sin_addr.s_addr = htonl(0x04040404); addr4.sin_port = htons(10004);
    addr5.sin_family = AF_INET; addr5.sin_addr.s_addr = htonl(0x05050505); addr5.sin_port = htons(10005);

    int slot1 = allocate_client_session(now, &addr1, false);
    int slot2 = allocate_client_session(now, &addr2, false);
    int slot3 = allocate_client_session(now, &addr3, true);
    int slot4 = allocate_client_session(now, &addr4, true);

    ASSERT_TRUE(slot1 >= 0 && slot1 < MAX_CLIENTS);
    ASSERT_TRUE(slot2 >= 0 && slot2 < MAX_CLIENTS);
    ASSERT_TRUE(slot3 >= 0 && slot3 < MAX_CLIENTS);
    ASSERT_TRUE(slot4 >= 0 && slot4 < MAX_CLIENTS);

    ASSERT_TRUE(slot1 != slot2);
    ASSERT_TRUE(slot1 != slot3);
    ASSERT_TRUE(slot1 != slot4);
    ASSERT_TRUE(slot2 != slot3);
    ASSERT_TRUE(slot2 != slot4);
    ASSERT_TRUE(slot3 != slot4);

    int slot5 = allocate_client_session(now, &addr5, false);
    ASSERT_EQ(slot5, -1);

    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[slot1]);
        ASSERT_TRUE(g_ctx.clients[slot1].active);
        ASSERT_EQ(g_ctx.clients[slot1].addr.sin_addr.s_addr, addr1.sin_addr.s_addr);
        ASSERT_TRUE(g_ctx.clients[slot1].first_pkt);
    }
    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[slot3]);
        ASSERT_TRUE(g_ctx.clients[slot3].uses_pad_presence);
    }

    reset_client_session(slot2);
    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[slot2]);
        ASSERT_TRUE(!g_ctx.clients[slot2].active);
    }

    int slot_reuse = allocate_client_session(now, &addr5, false);
    ASSERT_EQ(slot_reuse, slot2);

    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[slot1]);
        g_ctx.clients[slot1].last_rx_us = now + 999999999ULL;
        repair_future_client_timestamp(g_ctx.clients[slot1], now);
        ASSERT_EQ(g_ctx.clients[slot1].last_rx_us, now);
    }

    ASSERT_TRUE(any_recent_client_active(now));

    reset_server_state();
    std::cout << "[test] Client session management checks passed.\n";
}

void test_rate_limiter() {
    std::cout << "[test] Running rate limiter checks...\n";
    reset_server_state();

    uint32_t test_ip = 0xC0A80001;

    for (uint32_t i = 0; i < RATE_MAX_PKT; ++i) {
        ASSERT_TRUE(rate_allow(test_ip));
    }

    ASSERT_TRUE(!rate_allow(test_ip));

    uint32_t other_ip = 0xC0A80002;
    ASSERT_TRUE(rate_allow(other_ip));

    reset_server_state();
    std::cout << "[test] Rate limiter checks passed.\n";
}

void test_parse_client_packet() {
    std::cout << "[test] Running client packet parsing checks...\n";

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    {
        ns::Packet pkt{};
        pkt.magic = ns::PROTO_MAGIC;
        pkt.version = ns::PROTO_VERSION;
        pkt.flags = ns::FLAG_NONE;
        pkt.seq = 42;
        pkt.ts_us = ns::now_us();
        pkt.report.p1.buttons = ns::BTN_A;
        pkt.report.p1.lx = 200;
        pkt.report.p2.buttons = ns::BTN_B;

        hmac_sha256(
            std::span<const uint8_t>(hmac_key, 32),
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&pkt), ns::PACKET_AUTH_SIZE),
            std::span<uint8_t, 32>(pkt.hmac, 32)
        );

        uint8_t flags = 0;
        uint32_t seq = 0;
        ns::ExtendedMultiReport report{};
        ns::ExtendedMultiReport3 report3{};
        bool pad_present[4] = {};
        bool is_report3 = false;

        bool ok = parse_client_packet(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt),
                                       flags, seq, report, pad_present, is_report3, report3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(seq, 42u);
        ASSERT_EQ(flags, ns::FLAG_NONE);
        ASSERT_TRUE(!is_report3);
        ASSERT_EQ(report.p1.input.buttons, ns::BTN_A);
        ASSERT_EQ(report.p1.input.lx, 200);
        ASSERT_EQ(report.p2.input.buttons, ns::BTN_B);
        ASSERT_TRUE(pad_present[0]);
        ASSERT_TRUE(pad_present[1]);
    }

    {
        ExtendedUdpPacket ext_pkt{};
        ext_pkt.magic = ns::PROTO_MAGIC;
        ext_pkt.version = ns::WEB_PROTO_VERSION;
        ext_pkt.flags = ns::FLAG_NONE;
        ext_pkt.seq = 100;
        ext_pkt.timestamp_us = ns::now_us();
        ext_pkt.report.p1.input.buttons = ns::BTN_ZR;
        ext_pkt.report.p1.input.vendor = ns::EXT_PAD_PRESENT;
        ext_pkt.report.p1.has_motion = 1;
        ext_pkt.report.p1.motion.ax = 500;

        hmac_sha256(
            std::span<const uint8_t>(hmac_key, 32),
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&ext_pkt), EXT_UDP_PACKET_AUTH_SIZE),
            std::span<uint8_t, 32>(ext_pkt.hmac, 32)
        );

        uint8_t flags = 0;
        uint32_t seq = 0;
        ns::ExtendedMultiReport report{};
        ns::ExtendedMultiReport3 report3{};
        bool pad_present[4] = {};
        bool is_report3 = false;

        bool ok = parse_client_packet(reinterpret_cast<const uint8_t*>(&ext_pkt), sizeof(ext_pkt),
                                       flags, seq, report, pad_present, is_report3, report3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(seq, 100u);
        ASSERT_TRUE(!is_report3);
        ASSERT_EQ(report.p1.input.buttons, ns::BTN_ZR);
        ASSERT_TRUE(pad_present[0]);
        ASSERT_TRUE(!pad_present[1]);
    }

    {
        ExtendedUdpPacketPc ext3_pkt{};
        ext3_pkt.magic = ns::PROTO_MAGIC;
        ext3_pkt.version = ns::WEB_PROTO_VERSION_3;
        ext3_pkt.flags = ns::FLAG_NONE;
        ext3_pkt.seq = 200;
        ext3_pkt.timestamp_us = ns::now_us();
        ext3_pkt.report.p1.input.buttons = ns::BTN_HOME;
        ext3_pkt.report.p1.input.vendor = ns::EXT_PAD_PRESENT;
        ext3_pkt.report.p1.has_motion = 1;
        ext3_pkt.report.p1.motion[0].ax = 10;
        ext3_pkt.report.p1.motion[1].ax = 20;
        ext3_pkt.report.p1.motion[2].ax = 30;

        hmac_sha256(
            std::span<const uint8_t>(hmac_key, 32),
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&ext3_pkt), EXT3_UDP_PACKET_AUTH_SIZE),
            std::span<uint8_t, 32>(ext3_pkt.hmac, 32)
        );

        uint8_t flags = 0;
        uint32_t seq = 0;
        ns::ExtendedMultiReport report{};
        ns::ExtendedMultiReport3 report3{};
        bool pad_present[4] = {};
        bool is_report3 = false;

        bool ok = parse_client_packet(reinterpret_cast<const uint8_t*>(&ext3_pkt), sizeof(ext3_pkt),
                                       flags, seq, report, pad_present, is_report3, report3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(seq, 200u);
        ASSERT_TRUE(is_report3);
        ASSERT_EQ(report3.p1.input.buttons, ns::BTN_HOME);
        ASSERT_TRUE(pad_present[0]);
        ASSERT_EQ(report.p1.motion.ax, 30);
    }

    {
        ExtendedUdpPacket single_pkt{};
        single_pkt.magic = ns::PROTO_MAGIC;
        single_pkt.version = ns::WEB_PROTO_VERSION;
        single_pkt.flags = ns::FLAG_SINGLE_PAD;
        single_pkt.seq = 300;
        single_pkt.timestamp_us = ns::now_us();
        single_pkt.report.p1.input.buttons = ns::BTN_A;
        single_pkt.report.p1.input.vendor = ns::EXT_PAD_PRESENT;
        single_pkt.report.p2.input.buttons = ns::BTN_B;
        single_pkt.report.p2.input.vendor = ns::EXT_PAD_PRESENT;

        hmac_sha256(
            std::span<const uint8_t>(hmac_key, 32),
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&single_pkt), EXT_UDP_PACKET_AUTH_SIZE),
            std::span<uint8_t, 32>(single_pkt.hmac, 32)
        );

        uint8_t flags = 0;
        uint32_t seq = 0;
        ns::ExtendedMultiReport report{};
        ns::ExtendedMultiReport3 report3{};
        bool pad_present[4] = {};
        bool is_report3 = false;

        bool ok = parse_client_packet(reinterpret_cast<const uint8_t*>(&single_pkt), sizeof(single_pkt),
                                       flags, seq, report, pad_present, is_report3, report3);
        ASSERT_TRUE(ok);
        ASSERT_TRUE(pad_present[0]);
        ASSERT_TRUE(!pad_present[1]);
        ASSERT_TRUE(!pad_present[2]);
        ASSERT_TRUE(!pad_present[3]);
        ASSERT_EQ(report.p1.input.buttons, ns::BTN_A);
        ASSERT_EQ(report.p2.input.buttons, 0);
    }

    {
        uint8_t short_pkt[10] = {};
        uint8_t flags = 0;
        uint32_t seq = 0;
        ns::ExtendedMultiReport report{};
        ns::ExtendedMultiReport3 report3{};
        bool pad_present[4] = {};
        bool is_report3 = false;

        bool ok = parse_client_packet(short_pkt, sizeof(short_pkt),
                                       flags, seq, report, pad_present, is_report3, report3);
        ASSERT_TRUE(!ok);

        uint8_t bad_magic[68] = {};
        uint32_t bad = 0xDEADBEEF;
        memcpy(bad_magic, &bad, 4);
        ok = parse_client_packet(bad_magic, sizeof(bad_magic),
                                  flags, seq, report, pad_present, is_report3, report3);
        ASSERT_TRUE(!ok);
    }

    std::cout << "[test] Client packet parsing checks passed.\n";
}

void test_server_macro_execution() {
    std::cout << "[test] Running server-side macro execution checks...\n";
    reset_server_state();

    uint64_t now = ns::now_us();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x0A000001);
    addr.sin_port = htons(20001);
    int client = allocate_client_session(now, &addr, true);
    ASSERT_TRUE(client >= 0);

    bool started = server_macro_start(client, 0, "A 100\nB 150");
    ASSERT_TRUE(started);

    ASSERT_TRUE(server_macro_running(client, 0));

    ASSERT_TRUE(!server_macro_running(client, 1));
    ASSERT_TRUE(!server_macro_running(client, 2));
    ASSERT_TRUE(!server_macro_running(client, 3));

    ns::HIDReport live;
    live.reset();
    server_macro_apply(client, 0, live);
    ASSERT_EQ(live.buttons, ns::BTN_A);

    bool bad = server_macro_start(client, 0, "FAKE_BUTTON 999");
    ASSERT_TRUE(!bad);

    ASSERT_TRUE(!server_macro_start(-1, 0, "A 100"));
    ASSERT_TRUE(!server_macro_start(MAX_CLIENTS, 0, "A 100"));
    ASSERT_TRUE(!server_macro_running(-1, 0));
    ASSERT_TRUE(!server_macro_running(0, -1));

    server_macro_start(client, 1, "B 100");
    ASSERT_TRUE(server_macro_running(client, 1));
    server_macro_stop_all_for_client(client);
    ASSERT_TRUE(!server_macro_running(client, 0));
    ASSERT_TRUE(!server_macro_running(client, 1));

    reset_server_state();
    std::cout << "[test] Server-side macro execution checks passed.\n";
}

void test_rumble_state() {
    std::cout << "[test] Running rumble state checks...\n";
    reset_server_state();

    ns::RumblePacket rp{};
    rp.magic = ns::RUMBLE_MAGIC;
    rp.subpad = 2;
    rp.low_freq = 128;
    rp.high_freq = 200;
    rp.duration_10ms = 10;
    ASSERT_EQ(rp.magic, ns::RUMBLE_MAGIC);
    ASSERT_EQ(rp.subpad, 2);
    ASSERT_EQ(sizeof(rp), 8u);

    ns::PrecisionRumblePacket prp{};
    prp.magic = ns::PRECISION_RUMBLE_MAGIC;
    prp.subpad = 1;
    prp.low_freq = 64;
    prp.high_freq = 192;
    prp.duration_10ms = 20;
    ASSERT_EQ(prp.magic, ns::PRECISION_RUMBLE_MAGIC);
    ASSERT_EQ(sizeof(prp), 20u);

    uint64_t now = ns::now_us();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x0A000001);
    int slot = allocate_client_session(now, &addr, true);
    ASSERT_TRUE(slot >= 0);

    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[slot]);
        ClientSession& c = g_ctx.clients[slot];

        for (int s = 0; s < 4; ++s) {
            ASSERT_TRUE(!c.rumble_active[s]);
        }

        c.rumble[0] = rp;
        c.rumble[0].subpad = 0;
        c.rumble_active[0] = true;
        uint32_t old_seq = c.rumble_seq[0];
        c.rumble_seq[0]++;

        ASSERT_TRUE(c.rumble_active[0]);
        ASSERT_TRUE(c.rumble_seq[0] != old_seq);

        ns::RumblePacket neutral{};
        neutral.magic = ns::RUMBLE_MAGIC;
        neutral.subpad = 0;
        neutral.low_freq = 0;
        neutral.high_freq = 0;
        neutral.duration_10ms = 0;
        c.rumble[0] = neutral;
        c.rumble_active[0] = false;
        ASSERT_TRUE(!c.rumble_active[0]);

        ASSERT_TRUE(!c.udp_rumble_enabled);
        c.udp_rumble_enabled = true;
        ASSERT_TRUE(c.udp_rumble_enabled);
    }

    reset_server_state();
    std::cout << "[test] Rumble state checks passed.\n";
}

void test_switch2_wake_state() {
    std::cout << "[test] Running Switch 2 wake state machine checks...\n";
    reset_server_state();

    uint64_t now = ns::now_us();

    ASSERT_TRUE(!g_ctx.switch2_usb_host_connected.load());
    ASSERT_TRUE(!switch2_usb_host_recently_active(now));

    mark_switch2_usb_activity(now);
    ASSERT_TRUE(g_ctx.switch2_usb_host_connected.load());
    ASSERT_TRUE(switch2_usb_host_recently_active(now));

    uint64_t future = now + SWITCH2_USB_ACTIVITY_FRESH_US + 1;
    ASSERT_TRUE(!switch2_usb_host_recently_active(future));
    ASSERT_TRUE(!g_ctx.switch2_usb_host_connected.load());

    mark_switch2_usb_activity(ns::now_us());
    clear_switch2_usb_activity();
    ASSERT_TRUE(!g_ctx.switch2_usb_host_connected.load());
    ASSERT_EQ(g_ctx.switch2_last_usb_activity_us.load(), 0u);

    uint64_t seq_before = g_ctx.switch2_suspend_disconnect_seq.load();
    mark_switch2_usb_host_disconnected();
    ASSERT_TRUE(g_ctx.switch2_force_next_wake.load());
    ASSERT_EQ(g_ctx.switch2_suspend_disconnect_seq.load(), seq_before + 1);

    g_ctx.switch2_force_next_wake.store(false);
    clear_switch2_usb_activity();
    rearm_switch2_wake_after_client_disconnect();
    ASSERT_TRUE(g_ctx.switch2_force_next_wake.load());

    ASSERT_EQ(elapsed_us_saturated(1000, 500), 500u);
    ASSERT_EQ(elapsed_us_saturated(500, 1000), 0u);
    ASSERT_EQ(elapsed_us_saturated(1000, 0), 0u);
    ASSERT_TRUE(elapsed_us_over(1000, 500, 400));
    ASSERT_TRUE(!elapsed_us_over(1000, 500, 600));
    ASSERT_TRUE(!elapsed_us_over(1000, 0, 100));

    reset_server_state();
    std::cout << "[test] Switch 2 wake state machine checks passed.\n";
}

void test_bluetooth_helpers() {
    std::cout << "[test] Running Bluetooth helper function checks...\n";

    auto valid_mac = [](const std::string& mac) -> bool {
        if (mac.size() != 17) return false;
        for (size_t i = 0; i < 17; ++i)
            if (i % 3 == 2 ? mac[i] != ':' : !std::isxdigit((unsigned char)mac[i])) return false;
        return true;
    };
    auto valid_adv_hex = [](const std::string& adv) -> bool {
        return !adv.empty() && adv.size() % 2 == 0 && adv.size() <= 62 &&
               std::ranges::all_of(adv, [](char c) { return std::isxdigit((unsigned char)c); });
    };
    auto valid_hci = [](const std::string& hci) -> bool {
        return hci.size() >= 4 && hci.starts_with("hci") &&
               std::ranges::all_of(hci.begin() + 3, hci.end(), ::isdigit);
    };

    ASSERT_TRUE(valid_mac("AA:BB:CC:DD:EE:FF"));
    ASSERT_TRUE(valid_mac("00:09:bf:ab:cd:ef"));
    ASSERT_TRUE(!valid_mac(""));
    ASSERT_TRUE(!valid_mac("invalid"));
    ASSERT_TRUE(!valid_mac("AA:BB:CC:DD:EE:GG"));
    ASSERT_TRUE(!valid_mac("AA:BB:CC:DD:EE:FF:00"));
    ASSERT_TRUE(!valid_mac("AA:BB:CC:DD:EE:F"));

    ASSERT_TRUE(valid_adv_hex("0201061BFF5305"));
    ASSERT_TRUE(!valid_adv_hex(""));
    ASSERT_TRUE(!valid_adv_hex("0"));
    ASSERT_TRUE(!valid_adv_hex("0G"));
    ASSERT_TRUE(!valid_adv_hex(std::string(64, 'A')));

    ASSERT_TRUE(valid_hci("hci0"));
    ASSERT_TRUE(valid_hci("hci12"));
    ASSERT_TRUE(!valid_hci(""));
    ASSERT_TRUE(!valid_hci("hci"));
    ASSERT_TRUE(!valid_hci("abc0"));
    ASSERT_TRUE(!valid_hci("hcia"));

    auto to_lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    ASSERT_EQ(to_lower("ABCdef123!"), "abcdef123!");
    ASSERT_EQ(to_lower(""), "");

    auto to_upper_no_space = [](std::string s) {
        s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    };
    ASSERT_EQ(to_upper_no_space("02 01 06 1B FF"), "0201061BFF");
    ASSERT_EQ(to_upper_no_space("abc"), "ABC");
    ASSERT_EQ(to_upper_no_space(""), "");

    auto trim = [](std::string s) -> std::string {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        return s;
    };
    ASSERT_EQ(trim("  hello  "), "hello");
    ASSERT_EQ(trim("\n\t abc \r\n"), "abc");
    ASSERT_EQ(trim(""), "");
    ASSERT_EQ(trim("xyz"), "xyz");

    auto adv_hex_to_cmd_args = [&](const std::string& adv_hex) -> std::vector<std::string> {
        std::string adv = to_upper_no_space(adv_hex);
        size_t bytes = adv.size() / 2;
        std::vector<std::string> args = { std::format("{:02X}", (unsigned int)bytes) };
        for (size_t i = 0; i < 31; ++i)
            args.push_back(i < bytes ? adv.substr(i * 2, 2) : "00");
        return args;
    };
    auto args2 = adv_hex_to_cmd_args("020106");
    ASSERT_EQ(args2[0], "03");
    ASSERT_EQ(args2[1], "02");
    ASSERT_EQ(args2[2], "01");
    ASSERT_EQ(args2[3], "06");
    ASSERT_EQ(args2.size(), 32u);

    auto parse_nintendo_adv_from_btmon_log = [&](const std::string& path, const std::string& preferred_mac, std::string& out_mac, std::string& out_adv) -> bool {
        std::ifstream f(path);
        if (!f) return false;
        std::string line, cur_mac, preferred = to_lower(preferred_mac);
        while (std::getline(f, line)) {
            std::string lower = to_lower(line);
            size_t ap = lower.find("address:");
            if (ap != std::string::npos) {
                size_t p = ap + 8;
                while (p < line.size() && std::isspace((unsigned char)line[p])) ++p;
                if (p + 17 <= line.size()) {
                    std::string cand = to_lower(line.substr(p, 17));
                    if (valid_mac(cand)) cur_mac = cand;
                }
            }
            size_t dp = lower.find("data[24]:");
            if (dp == std::string::npos) continue;
            size_t p = line.find(':', dp);
            if (p == std::string::npos) continue;
            std::string data = to_upper_no_space(line.substr(p + 1));
            if (data.size() != 48 || cur_mac.empty() || (!preferred.empty() && cur_mac != preferred)) continue;
            out_mac = cur_mac;
            out_adv = "0201061BFF5305" + data;
            if (valid_adv_hex(out_adv)) return true;
        }
        return false;
    };

    {
        std::string log_content =
            "> ACL Data RX: Handle 42 flags 0x02 dlen 10\n"
            "      address: AA:BB:CC:DD:EE:FF\n"
            "      data[24]: 11223344556677889900AABBCCDDEEFF1122334455667788\n";
        std::string path = "/tmp/test_btmon_parse_XXXXXX";
        {
            std::ofstream f(path);
            f << log_content;
        }
        std::string out_mac, out_adv;
        bool ok = parse_nintendo_adv_from_btmon_log(path, "AA:BB:CC:DD:EE:FF", out_mac, out_adv);
        ASSERT_TRUE(ok);
        ASSERT_EQ(out_mac, "aa:bb:cc:dd:ee:ff");
        ASSERT_TRUE(valid_adv_hex(out_adv));
        ASSERT_EQ(out_adv, "0201061BFF530511223344556677889900AABBCCDDEEFF1122334455667788");
        std::filesystem::remove(path);
    }

    {
        std::string out_mac, out_adv;
        bool ok = parse_nintendo_adv_from_btmon_log("/tmp/nonexistent_log_file", "", out_mac, out_adv);
        ASSERT_TRUE(!ok);
    }

    std::cout << "[test] Bluetooth helper function checks passed.\n";
}

void test_bluetooth_input_available() {
    std::cout << "[test] Running bluetooth_input_available() check...\n";
    ASSERT_TRUE(bluetooth_input_available());
    std::cout << "[test] bluetooth_input_available() check passed.\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "Starting NS-PC-Control Unit Test Suite\n";
    std::cout << "========================================\n";

    test_crypto();
    test_protocol_sizes();
    test_macro_parsing();
    test_macro_recorder_runtime();
    test_sdl_input_helpers();

    test_protocol_helpers();
    test_all_button_tokens();
    test_macro_edge_cases();
    test_macro_entry_system();
    test_sdl_input_extended();

    test_client_session_management();
    test_rate_limiter();
    test_parse_client_packet();
    test_server_macro_execution();
    test_rumble_state();
    test_switch2_wake_state();

    test_bluetooth_helpers();
    test_bluetooth_input_available();

    std::cout << "========================================\n";
    std::cout << "ALL 18 UNIT TESTS COMPLETED SUCCESSFULLY!\n";
    std::cout << "========================================\n";
    return 0;
}

#include "shared/protocol.hpp"
#include "shared/macros.hpp"
#include "shared/sha256.h"

#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <span>

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

    // 1. Key derivation
    uint8_t key1[32];
    uint8_t key2[32];
    derive_key("my_super_secret_test_key", key1);
    derive_key("my_super_secret_test_key", key2);

    // Verify consistency
    for (int i = 0; i < 32; ++i) {
        ASSERT_EQ(key1[i], key2[i]);
    }

    // Different secrets should yield different keys
    uint8_t key_diff[32];
    derive_key("another_key", key_diff);
    bool distinct = false;
    for (int i = 0; i < 32; ++i) {
        if (key1[i] != key_diff[i]) distinct = true;
    }
    ASSERT_TRUE(distinct);

    // 2. HMAC Generation & Verification
    uint8_t msg[] = "Hello World! This is a test input payload.";
    uint8_t out[32];
    hmac_sha256(std::span<const uint8_t>(key1, 32), std::span<const uint8_t>(msg, sizeof(msg)), std::span<uint8_t, 32>(out, 32));

    // Verify matching
    int verify_ok = hmac_verify(std::span<const uint8_t>(key1, 32), std::span<const uint8_t>(msg, sizeof(msg)), std::span<const uint8_t>(out, 32));
    ASSERT_EQ(verify_ok, 0);

    // Verify verification fails with altered message
    msg[0] = 'h'; // alter msg
    int verify_bad = hmac_verify(std::span<const uint8_t>(key1, 32), std::span<const uint8_t>(msg, sizeof(msg)), std::span<const uint8_t>(out, 32));
    ASSERT_TRUE(verify_bad != 0);

    // Reset message, verify verification fails with wrong key
    msg[0] = 'H';
    int verify_bad_key = hmac_verify(std::span<const uint8_t>(key_diff, 32), std::span<const uint8_t>(msg, sizeof(msg)), std::span<const uint8_t>(out, 32));
    ASSERT_TRUE(verify_bad_key != 0);

    std::cout << "[test] Cryptography checks passed.\n";
}

void test_protocol_sizes() {
    std::cout << "[test] Running wire-format and size verification checks...\n";

    // Standard structural requirements
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

    // 1. String utilities
    ASSERT_EQ(ns::macro::trim("  hello world  "), "hello world");
    ASSERT_EQ(ns::macro::trim("\n\t  abc  \r\n"), "abc");
    ASSERT_EQ(ns::macro::upper("aBcDef123!"), "ABCDEF123!");

    // 2. Strict uint32 parsing
    uint32_t val = 0;
    ASSERT_TRUE(ns::macro::parse_uint32_strict("123456", val));
    ASSERT_EQ(val, 123456);
    ASSERT_TRUE(ns::macro::parse_uint32_strict("0", val));
    ASSERT_EQ(val, 0);
    ASSERT_TRUE(!ns::macro::parse_uint32_strict("123a", val));
    ASSERT_TRUE(!ns::macro::parse_uint32_strict("-123", val));
    ASSERT_TRUE(!ns::macro::parse_uint32_strict("", val));
    ASSERT_TRUE(!ns::macro::parse_uint32_strict("4294967296", val)); // overflow 2^32

    // 3. Button token resolution
    ASSERT_EQ(ns::macro::button_bit("A"), ns::BTN_A);
    ASSERT_EQ(ns::macro::button_bit("BTN_A"), ns::BTN_A);
    ASSERT_EQ(ns::macro::button_bit("X"), ns::BTN_X);
    ASSERT_EQ(ns::macro::button_bit("ZL"), ns::BTN_ZL);
    ASSERT_EQ(ns::macro::button_bit("MINUS"), ns::BTN_MINUS);
    ASSERT_EQ(ns::macro::button_bit("invalid_button"), 0);

    // 4. Macro JSON extracting
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

    // 5. Script parsing and validation
    std::string test_script = "WAIT 100\nA 250\nR+LSTICK_LEFT 400\nLOOP 2";
    auto parsed_steps = ns::macro::parse_text(test_script);
    ASSERT_TRUE(!parsed_steps.empty());

    // Verify first step (WAIT 100)
    ASSERT_EQ(parsed_steps[0].duration_ms, 100);
    ASSERT_EQ(parsed_steps[0].buttons, 0);

    // Verify second step (A 250)
    ASSERT_EQ(parsed_steps[1].duration_ms, 250);
    ASSERT_EQ(parsed_steps[1].buttons, ns::BTN_A);

    // Verify third step (R+LSTICK_LEFT 400)
    ASSERT_EQ(parsed_steps[2].duration_ms, 400);
    ASSERT_EQ(parsed_steps[2].buttons, ns::BTN_R);
    ASSERT_TRUE(parsed_steps[2].has_lstick);
    ASSERT_EQ(parsed_steps[2].lx, 0); // left full deflection
    ASSERT_EQ(parsed_steps[2].ly, 128); // neutral

    // 6. Pretty JSON serialization
    std::string pretty_out, err_out;
    bool pretty_ok = ns::macro::validate_to_pretty_json("A 100\nB 200", pretty_out, err_out, "Test Name");
    ASSERT_TRUE(pretty_ok);
    ASSERT_TRUE(pretty_out.find("\"name\": \"Test Name\"") != std::string::npos);
    ASSERT_TRUE(pretty_out.find("\"commands\"") != std::string::npos);

    std::cout << "[test] Macro parser and validation checks passed.\n";
}

void test_macro_recorder_runtime() {
    std::cout << "[test] Running macro recorder and playback checks...\n";

    // 1. Recorder
    ns::macro::Recorder rec;
    rec.start(1000);
    
    // Add sample reports
    ns::HIDReport r1;
    r1.buttons = ns::BTN_A;
    rec.sample(r1, 1000); // T = 0ms
    
    ns::HIDReport r2;
    r2.buttons = ns::BTN_B;
    rec.sample(r2, 100000); // T = 100ms
    
    std::string macro_result = rec.stop(250000); // T = 250ms
    ASSERT_TRUE(!macro_result.empty());
    
    // 2. Playback Runtime
    ns::macro::Runtime rt;
    bool started = ns::macro::runtime_start_text(rt, "A 100\nB 150", 1000);
    ASSERT_TRUE(started);
    ASSERT_TRUE(ns::macro::runtime_running(rt, 1000));
    
    ns::HIDReport out_rep;
    // Check at elapsed T = 50ms (first step is active)
    bool playing = ns::macro::runtime_report(rt, 1000 + 50000, out_rep);
    ASSERT_TRUE(playing);
    ASSERT_EQ(out_rep.buttons, ns::BTN_A);
    
    // Check at elapsed T = 150ms (second step is active)
    playing = ns::macro::runtime_report(rt, 1000 + 150000, out_rep);
    ASSERT_TRUE(playing);
    ASSERT_EQ(out_rep.buttons, ns::BTN_B);

    // Check after completion
    playing = ns::macro::runtime_report(rt, 1000 + 350000, out_rep);
    ASSERT_TRUE(!playing);

    std::cout << "[test] Macro recorder and playback checks passed.\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "Starting NS-PC-Control Unit Test Suite\n";
    std::cout << "========================================\n";

    test_crypto();
    test_protocol_sizes();
    test_macro_parsing();
    test_macro_recorder_runtime();

    std::cout << "========================================\n";
    std::cout << "ALL UNIT TESTS COMPLETED SUCCESSFULLY!\n";
    std::cout << "========================================\n";
    return 0;
}

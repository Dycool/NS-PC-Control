#pragma once

#include "app_state.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <sys/types.h>
#include <string>

uint8_t pro_timer_from_us(uint64_t t_us);

constexpr size_t PRO_REPORT_SIZE = 64;
constexpr uint64_t PRO_REPORT_INTERVAL_US = 4'000ULL;
constexpr int PRO_WRITER_HZ = 1'000'000 / PRO_REPORT_INTERVAL_US;
constexpr uint8_t PRO_BAT_CON = 0x91;
constexpr uint8_t PRO_VIBRATOR_REPORT = 0x0B;
constexpr int PRO_IDLE_REPORT_HZ = 30;
constexpr uint64_t PRO_IDLE_REPORT_INTERVAL_US = 1'000'000ULL / PRO_IDLE_REPORT_HZ;
constexpr uint64_t PRO_RELEASE_NEUTRAL_US = 250'000ULL;
constexpr uint64_t WEB_PAD_ABSENT_RELEASE_US = 750'000ULL;
constexpr uint8_t RID_INPUT_STANDARD = 0x30;
constexpr uint8_t RID_INPUT_SUBCMD = 0x21;
constexpr uint8_t RID_OUTPUT_RUMBLE = 0x10;
constexpr uint8_t RID_OUTPUT_CMD = 0x01;

constexpr uint8_t CMD_BT_MANUAL_PAIRING = 0x01;
constexpr uint8_t CMD_GET_DEVICE_INFO = 0x02;
constexpr uint8_t CMD_SET_DATA_FORMAT = 0x03;
constexpr uint8_t CMD_TRIGGER_BUTTONS = 0x04;
constexpr uint8_t CMD_SET_SHIP_MODE = 0x08;
constexpr uint8_t CMD_SPI_FLASH_READ = 0x10;
constexpr uint8_t CMD_SET_NFC_IR_CONFIG = 0x21;
constexpr uint8_t CMD_SET_PLAYER_LIGHTS = 0x30;
constexpr uint8_t CMD_ENABLE_IMU = 0x40;
constexpr uint8_t CMD_SET_IMU_SENS = 0x41;
constexpr uint8_t CMD_ENABLE_VIBRATION = 0x48;

extern const uint8_t VIRTUAL_CONTROLLER_REPORT_DESC[203];
extern const uint8_t LEGACY_REPORT_DESC[85];

#define NS_LOCAL_PACKED __attribute__((packed))

struct NS_LOCAL_PACKED ProInputReport30 {
    uint8_t id;
    uint8_t timer;
    uint8_t conn_info;
    uint8_t buttons[3];
    uint8_t left_stick[3];
    uint8_t right_stick[3];
    uint8_t vibrator;
    int16_t accel_y_0, accel_x_0, accel_z_0;
    int16_t gyro_y_0, gyro_x_0, gyro_z_0;
    int16_t accel_y_1, accel_x_1, accel_z_1;
    int16_t gyro_y_1, gyro_x_1, gyro_z_1;
    int16_t accel_y_2, accel_x_2, accel_z_2;
    int16_t gyro_y_2, gyro_x_2, gyro_z_2;
    uint8_t vendor_rest[15];
};
static_assert(sizeof(ProInputReport30) == PRO_REPORT_SIZE, "ProInputReport30 must be 64 bytes");

struct NS_LOCAL_PACKED ProInputReport21 {
    uint8_t id;
    uint8_t timer;
    uint8_t conn_info;
    uint8_t buttons[3];
    uint8_t left_stick[3];
    uint8_t right_stick[3];
    uint8_t vibrator;
    uint8_t ack;
    uint8_t subcmd_id;
    uint8_t reply_data[49];
};
static_assert(sizeof(ProInputReport21) == PRO_REPORT_SIZE, "ProInputReport21 must be 64 bytes");

extern uint8_t CTRL_MAC_BE[4][6];
extern std::string CTRL_SERIAL[4];

struct ControllerRuntime {
    int fd = -1;
    int ctrl = 0;
    uint8_t timer = 0;
    bool full_report_enabled = false;
    bool imu_enabled = false;
    bool vibration_enabled = false;
    bool usb_seen_mac = false;
    bool usb_handshake_done = false;
    bool usb_baudrate_set = false;
    bool usb_timeout_disabled = false;
    bool pending_subcmd_reply = false;
    uint64_t last_standard_report_us = 0;
    uint64_t last_idle_neutral_us = 0;
    uint64_t neutral_burst_until_us = 0;
    ProInputReport21 pending_reply{};
};

void randomize_controller_identity();
void init_spi_flash(int ctrl);
size_t build_usb_81_response(uint8_t* out, uint8_t subtype, int ctrl);
void build_get_device_info_response(uint8_t* out, int ctrl);
void fill_neutral_controls(ProInputReport30& r);
void fill_neutral_controls(ProInputReport21& r);
uint16_t axis8_to_12(uint8_t v);
uint8_t invert_axis8_centered(uint8_t v);
void pack_stick_12(uint8_t out[3], uint8_t x8, uint8_t y8);
bool input_is_neutral(const ns::HIDReport& r);
bool motion_is_neutral(const ns::MotionReport& m);
bool extended_is_neutral(const ns::ExtendedHIDReport& r);
void hat_to_pro_buttons(uint8_t hat, uint8_t buttons[3]);
void apply_input_controls_to_pro21(const ns::ExtendedHIDReport& src, ProInputReport21& out);
void build_standard_report(const ns::ExtendedHIDReport& src,
                           const ns::MotionReport motion_samples[3],
                           bool has_motion,
                           bool imu_enabled,
                           uint8_t timer,
                           ProInputReport30& out);
int handle_subcommand(ControllerRuntime& rt, uint8_t subcmd, std::span<const uint8_t> cmd_data, ProInputReport21* reply);
void publish_rumble_event(int client_idx, int sub_idx, const uint8_t* packet, ssize_t len, bool publish_neutral);

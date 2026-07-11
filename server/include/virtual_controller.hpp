#pragma once

#include "app_state.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <mutex>
#include <span>
#include <sys/types.h>
#include <string>

uint8_t pro_timer_from_us(uint64_t t_us);

constexpr size_t PRO_REPORT_SIZE = 64;
constexpr size_t HIDG_MAX_REPORT_SIZE = PRO_REPORT_SIZE;
constexpr uint64_t PRO_REPORT_INTERVAL_US = 4'000ULL;
constexpr int PRO_WRITER_HZ = 1'000'000 / PRO_REPORT_INTERVAL_US;
// Default Pro Controller battery/connection byte: full battery, connected, not charging.
constexpr uint8_t PRO_BAT_CON = 0x81;
constexpr uint8_t PRO_VIBRATOR_REPORT = 0x0B;
constexpr int PRO_IDLE_REPORT_HZ = 30;
constexpr uint64_t PRO_IDLE_REPORT_INTERVAL_US = 1'000'000ULL / PRO_IDLE_REPORT_HZ;
constexpr uint64_t PRO_RELEASE_NEUTRAL_US = 250'000ULL;
constexpr uint64_t WEB_PAD_ABSENT_RELEASE_US = 750'000ULL;
constexpr uint8_t RID_INPUT_STANDARD = 0x30;
constexpr uint8_t RID_INPUT_SUBCMD = 0x21;
constexpr uint8_t RID_OUTPUT_RUMBLE = 0x10;
constexpr uint8_t RID_OUTPUT_CMD = 0x01;
constexpr uint8_t NS_TYPE_JOYCON_L = 0x01;
constexpr uint8_t NS_TYPE_JOYCON_R = 0x02;
constexpr uint8_t NS_TYPE_PRO      = 0x03;
constexpr uint8_t NS_TYPE_HORI      = 0x04;
 
constexpr uint8_t CMD_BT_MANUAL_PAIRING = 0x01;
constexpr uint8_t CMD_GET_DEVICE_INFO = 0x02;
constexpr uint8_t CMD_SET_DATA_FORMAT = 0x03;
constexpr uint8_t CMD_TRIGGER_BUTTONS = 0x04;
constexpr uint8_t CMD_SET_SHIP_MODE = 0x08;
constexpr uint8_t CMD_SPI_FLASH_READ = 0x10;
constexpr uint8_t CMD_SET_PLAYER_LIGHTS = 0x30;
constexpr uint8_t CMD_ENABLE_IMU = 0x40;
constexpr uint8_t CMD_SET_IMU_SENS = 0x41;
constexpr uint8_t CMD_ENABLE_VIBRATION = 0x48;
 
extern const uint8_t VIRTUAL_CONTROLLER_REPORT_DESC[];
extern const size_t VIRTUAL_CONTROLLER_REPORT_DESC_SIZE;
extern const uint8_t LEGACY_REPORT_DESC[85];

extern const uint8_t S2_PRO_REPORT_DESC[];
extern const size_t S2_PRO_REPORT_DESC_SIZE;

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
extern const uint8_t VIRTUAL_BODY_RGB[4][3];

struct ControllerRuntime {
    int fd = -1;
    int ctrl = 0;
    uint8_t timer = 0;
    uint8_t input_report_mode = RID_INPUT_STANDARD;
    bool full_report_enabled = false;
    bool imu_enabled = false;
    bool vibration_enabled = false;
    bool usb_seen_mac = false;
    bool usb_handshake_done = false;
    bool usb_baudrate_set = false;
    bool usb_timeout_disabled = false;
    bool pending_subcmd_reply = false;
    bool pending_cmd_response = false;
    uint8_t cmd_response_buf[64] = {};
    size_t cmd_response_len = 0;
    uint64_t last_standard_report_us = 0;
    uint64_t last_idle_neutral_us = 0;
    uint64_t neutral_burst_until_us = 0;
    ProInputReport21 pending_reply{};
};

void randomize_controller_identity();
void configure_usb_controller_family(UsbControllerFamily family);
void init_spi_flash(int ctrl);
size_t build_usb_81_response(uint8_t* out, uint8_t subtype, int ctrl);
void build_get_device_info_response(uint8_t* out, int ctrl);
void fill_neutral_controls(ProInputReport30& r);
void fill_neutral_controls(ProInputReport21& r);
uint8_t pro_conn_info_from_hid(const ns::HIDReport& src);
bool player_leds_indicate_pairing(uint8_t player_leds);
uint16_t axis8_to_12(uint8_t v);
uint8_t invert_axis8_centered(uint8_t v);
void pack_stick_12(uint8_t out[3], uint8_t x8, uint8_t y8);
bool input_is_neutral(const ns::HoriHIDReport& r);
bool motion_is_neutral(const ns::MotionReport& m);
bool hid_is_neutral(const ns::HIDReport& r);
void hat_to_pro_buttons(uint8_t hat, uint8_t buttons[3]);
void apply_input_controls_to_pro21(const ns::HIDReport& src, ProInputReport21& out);
void build_standard_report(const ns::HIDReport& src,
                           const ns::MotionReport motion_samples[3],
                           bool has_motion,
                           bool imu_enabled,
                           uint8_t timer,
                           ProInputReport30& out,
                           bool is_switch2 = false);

struct S2JoyconMouseInput {
    int16_t dx = 0;
    int16_t dy = 0;
    uint8_t surface = 0;
    bool active = false;
};

void build_s2_pro_report(const ns::HIDReport& src,
                         const ns::MotionReport motion_samples[3],
                         bool has_motion,
                         bool imu_enabled,
                         uint8_t timer,
                         uint64_t motion_time_us,
                         int port,
                         uint8_t* out,
                         const S2JoyconMouseInput* mouse = nullptr);
void reset_s2_motion_state(int port);
int handle_subcommand(ControllerRuntime& rt, uint8_t subcmd, std::span<const uint8_t> cmd_data, ProInputReport21* reply);
int handle_s2_subcommand(ControllerRuntime& rt, uint8_t subcmd, std::span<const uint8_t> cmd_data, ProInputReport21* reply);
size_t fill_nfc_response_payload(uint8_t nfc_sub, std::span<const uint8_t> cmd_data, uint8_t* payload, int port);
void publish_rumble_event(int client_idx, int sub_idx, const uint8_t* packet, ssize_t len, bool publish_neutral);
void publish_s2_rumble_event(int client_idx, int sub_idx, const uint8_t* packet, ssize_t len, bool publish_neutral);

uint8_t controller_type_for_port(int ctrl);
void set_controller_type_for_port(int ctrl, uint8_t protocol_type);
// The console reads each port's identity (device info subcmd 0x02, SPI 0x6012)
// only during the USB handshake, so an S1 type change after enumeration is
// invisible until the gadget re-enumerates. The writer polls this and forces
// one debounced re-enumeration when the live types differ from the ones the
// console last read.
bool s1_identity_reenumeration_due(uint64_t now);
void mark_s1_identity_enumerated();
constexpr uint64_t S1_TYPE_REENUM_QUIET_US = 1'000'000ULL;
uint8_t controller_protocol_type_for_port(int ctrl);
uint8_t switch2_input_report_id_for_port(int ctrl);
uint8_t switch2_output_report_id_for_port(int ctrl);
bool controller_port_supports_amiibo(int ctrl);
void apply_controller_type_input(uint8_t type, ns::HIDReport& r, bool pair_member = false);
void apply_controller_type_report(uint8_t type, uint8_t extra_buttons, uint8_t* buf);
void apply_s2_controller_type_report(uint8_t type, uint8_t* buf);

bool is_amiibo_placed(int port);
uint8_t amiibo_nfc_report_state(int port);
void set_amiibo_data_for_port(int port, const uint8_t* data, size_t len);
void check_amiibo_expiry(int port);

// Per-port Switch 2 flag (set when S2 protocol type used)
extern bool g_port_switch2[HID_PORT_COUNT];

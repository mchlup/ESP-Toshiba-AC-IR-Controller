#ifndef TOSHIBA_IR_GENERATOR_H
#define TOSHIBA_IR_GENERATOR_H

#include <Arduino.h>
#include "ToshibaCarrierHvac.h"

// Simple generator for Toshiba RAS A/C IR codes (F2 0D 03 FC 01 ... 9-byte frame).
// It produces the 72-bit "state" frame used by a lot of Toshiba / Carrier
// units and can optionally pack it into the 32+32+8-bit layout that some
// IR libraries treat as "Samsung-style".

namespace ToshibaIR {

/// Protocol family we generate. (Kept as enum for future extensions.)
enum ProtocolVariant : uint8_t {
    PROTO_TOSHIBA_AC_72BIT = 0,       ///< 9 data bytes (72 bits) + XOR checksum.
    PROTO_TOSHIBA_AC_72BIT_PACKED     ///< Same bits, exposed as 32+32+8.
};

/// Operating modes as encoded in the low 3 bits of Byte[6].
enum Mode : uint8_t {
    MODE_AUTO = 0,  ///< 0b000
    MODE_COOL = 1,  ///< 0b001
    MODE_DRY  = 2,  ///< 0b010
    MODE_HEAT = 3,  ///< 0b011
    MODE_FAN  = 4,  ///< 0b100
    MODE_OFF  = 7   ///< 0b111 (special "off" code)
};

/// Fan speeds as encoded in the high 3 bits of Byte[6].
enum Fan : uint8_t {
    FAN_AUTO = 0,   ///< 0b000
    FAN_1    = 1,   ///< 0b001
    FAN_2    = 2,   ///< 0b010
    FAN_3    = 3,   ///< 0b011
    FAN_4    = 4,   ///< 0b100
    FAN_5    = 5    ///< 0b101
};

/// Swing commands encoded in the low bits of Byte[5].
enum Swing : uint8_t {
    SWING_STEP   = 0,  ///< 0b000 - single step / position
    SWING_ON     = 1,  ///< 0b001 - continuous swing on
    SWING_OFF    = 2,  ///< 0b010 - swing off
    SWING_TOGGLE = 4   ///< 0b100 - toggle swing
};

/// Raw 9-byte (72-bit) Toshiba state frame.
struct Frame72 {
    uint8_t data[9];
};

/// Alternative representation used by some IR libraries that see this as
/// a "Samsung-style" 72-bit frame: first 32 bits, second 32 bits, 8-bit checksum.
struct PackedSamsung72 {
    uint32_t first32;   ///< bytes[0..3], MSB first (e.g. 0xF20D03FC)
    uint32_t second32;  ///< bytes[4..7], MSB first
    uint8_t  checksum;  ///< XOR of bytes[4..7] (and thus also of bytes[0..7])
};

/// Build a 72-bit Toshiba IR frame directly from basic parameters.
///
/// \param mode      Operating mode (AUTO/COOL/HEAT/DRY/FAN).
/// \param tempC     Setpoint in °C (clamped to 17..30).
/// \param fan       Fan speed (AUTO / 1..5).
/// \param powerOn   If false, an "off" frame is generated (MODE_OFF).
/// \param swing     Simple swing command (used in low bits of Byte[5]).
/// \param filter    Pure / ion filter flag (Byte[7] bit 4).
Frame72 buildFromParams(Mode mode,
                        uint8_t tempC,
                        Fan fan,
                        bool powerOn,
                        Swing swing = SWING_OFF,
                        bool filter = false);

/// Build a 72-bit frame from the hvacSettings struct used by ToshibaCarrierHvac.
///
/// Only common fields are mapped:
///   - state:   "on"/"off"
///   - setpoint (°C)
///   - mode:    "auto", "cool", "heat", "dry", "fan_only"
///   - fanMode: "quiet", "lvl_1".."lvl_5", "auto"
///   - swing:   "fix", "v_swing", "h_swing", "hv_swing", "fix_pos_X"
///   - pure:    "off"/"on"
Frame72 buildFromHvacSettings(const hvacSettings &settings);

/// Pack the 9-byte frame into (first32,second32,checksum) layout.
PackedSamsung72 packSamsung(const Frame72 &frame);

/// Compute XOR checksum of bytes[0..7].
uint8_t computeChecksum(const Frame72 &frame);

} // namespace ToshibaIR

#endif // TOSHIBA_IR_GENERATOR_H

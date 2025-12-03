#include "ToshibaIrGenerator.h"

#include <string.h>
#ifdef __has_include
#  if __has_include(<strings.h>)
#    include <strings.h>  // for strcasecmp on some platforms
#  endif
#endif

namespace ToshibaIR {

// --- local helpers ---------------------------------------------------------

static Mode modeFromString(const char *modeStr) {
    if (!modeStr) return MODE_AUTO;

    if (strcasecmp(modeStr, "cool") == 0)      return MODE_COOL;
    if (strcasecmp(modeStr, "heat") == 0)      return MODE_HEAT;
    if (strcasecmp(modeStr, "dry") == 0)       return MODE_DRY;
    if (strcasecmp(modeStr, "fan_only") == 0)  return MODE_FAN;
    if (strcasecmp(modeStr, "auto") == 0)      return MODE_AUTO;

    // Fallback: keep previously set mode on the unit, but we can't
    // know it here, so default to AUTO.
    return MODE_AUTO;
}

static Fan fanFromString(const char *fanStr) {
    if (!fanStr) return FAN_AUTO;

    if (strcasecmp(fanStr, "lvl_1") == 0) return FAN_1;
    if (strcasecmp(fanStr, "lvl_2") == 0) return FAN_2;
    if (strcasecmp(fanStr, "lvl_3") == 0) return FAN_3;
    if (strcasecmp(fanStr, "lvl_4") == 0) return FAN_4;
    if (strcasecmp(fanStr, "lvl_5") == 0) return FAN_5;

    // On the wired protocol "quiet" is a dedicated mode, but the IR
    // protocol doesn't have a 6th speed. Treat it as AUTO here.
    if (strcasecmp(fanStr, "quiet") == 0) return FAN_AUTO;
    if (strcasecmp(fanStr, "auto") == 0)  return FAN_AUTO;

    return FAN_AUTO;
}

static Swing swingFromString(const char *swingStr) {
    if (!swingStr) return SWING_OFF;

    // Nově: "auto" chápeme jako kontinuální swing.
    if (strcasecmp(swingStr, "auto") == 0) {
        return SWING_ON;
    }

    // Basic mapping: any "*swing*" means we ask for continuous swing.
    if (strstr(swingStr, "swing") != nullptr) {
        return SWING_ON;
    }

    // "fix" and "fix_pos_X" map to swing off.
    return SWING_OFF;
}

static bool filterFromPure(const char *pureStr) {
    if (!pureStr) return false;
    return strcasecmp(pureStr, "on") == 0;
}

// --- public API ------------------------------------------------------------

Frame72 buildFromParams(Mode mode,
                        uint8_t tempC,
                        Fan fan,
                        bool powerOn,
                        Swing swing,
                        bool filter) {
    Frame72 frame;
    // Clear just in case.
    for (uint8_t &b : frame.data) b = 0;

    // Fixed header for 72-bit "state" messages.
    frame.data[0] = 0xF2;
    frame.data[1] = 0x0D;
    frame.data[2] = 0x03;                 // length (3 bytes of payload after Byte[4])
    frame.data[3] = static_cast<uint8_t>(~frame.data[2]);  // inverted length (0xFC)
    frame.data[4] = 0x01;                 // message flags: short state frame

    // Temperature: 17..30 °C -> 0..13 encoded in high nibble of Byte[5].
    if (tempC < 17) tempC = 17;
    if (tempC > 30) tempC = 30;
    uint8_t tempIndex = tempC - 17;

    uint8_t swingCode = 0;
    switch (swing) {
        case SWING_ON:     swingCode = 1; break;
        case SWING_OFF:    swingCode = 2; break;
        case SWING_TOGGLE: swingCode = 4; break;
        case SWING_STEP:
        default:           swingCode = 0; break;
    }

    frame.data[5] = static_cast<uint8_t>((tempIndex << 4) | (swingCode & 0x0F));

    // Encode mode & power. If power is off we send the special "off" mode.
    Mode effMode = powerOn ? mode : MODE_OFF;

    uint8_t modeBits;
    switch (effMode) {
        case MODE_AUTO: modeBits = 0; break;
        case MODE_COOL: modeBits = 1; break;
        case MODE_DRY:  modeBits = 2; break;
        case MODE_HEAT: modeBits = 3; break;
        case MODE_FAN:  modeBits = 4; break;
        case MODE_OFF:
        default:        modeBits = 7; break;
    }

    uint8_t fanBits;
    switch (fan) {
        case FAN_1:    fanBits = 1; break;
        case FAN_2:    fanBits = 2; break;
        case FAN_3:    fanBits = 3; break;
        case FAN_4:    fanBits = 4; break;
        case FAN_5:    fanBits = 5; break;
        case FAN_AUTO:
        default:       fanBits = 0; break;
    }

    // Byte[6]: high 3 bits = fan, low 3 bits = mode.
    frame.data[6] = static_cast<uint8_t>(((fanBits & 0x07) << 5) | (modeBits & 0x07));

    // Byte[7]: Filter flag in bit 4 (0x10) for long/extended messages.
    // For simple 72-bit state frames this bit is still honored by many units.
    frame.data[7] = filter ? 0x10 : 0x00;

    // Byte[8]: XOR checksum over bytes[0..7].
    frame.data[8] = computeChecksum(frame);

    return frame;
}

Frame72 buildFromHvacSettings(const hvacSettings &settings) {
    Mode mode = modeFromString(settings.mode);
    Fan fan   = fanFromString(settings.fanMode);
    Swing swing = swingFromString(settings.swing);
    bool filter = filterFromPure(settings.pure);

    bool powerOn = true;
    if (settings.state && strcasecmp(settings.state, "off") == 0) {
        powerOn = false;
    }
    // Some firmwares also expose "operation" as "off" / "stop".
    if (settings.operation &&
        (strcasecmp(settings.operation, "off") == 0 ||
         strcasecmp(settings.operation, "stop") == 0)) {
        powerOn = false;
    }

    uint8_t tempC = settings.setpoint;
    if (tempC == 0) {
        // fall back to a sane default if not initialised
        tempC = 24;
    }

    return buildFromParams(mode, tempC, fan, powerOn, swing, filter);
}

PackedSamsung72 packSamsung(const Frame72 &frame) {
    PackedSamsung72 out{};
    const uint8_t *b = frame.data;

    // Big-endian packing: bytes[0] is the MSB.
    out.first32  = (static_cast<uint32_t>(b[0]) << 24) |
                   (static_cast<uint32_t>(b[1]) << 16) |
                   (static_cast<uint32_t>(b[2]) << 8)  |
                   (static_cast<uint32_t>(b[3]));

    out.second32 = (static_cast<uint32_t>(b[4]) << 24) |
                   (static_cast<uint32_t>(b[5]) << 16) |
                   (static_cast<uint32_t>(b[6]) << 8)  |
                   (static_cast<uint32_t>(b[7]));

    // In this protocol the XOR of bytes[0..3] is zero, so the checksum of
    // bytes[0..7] is also the checksum of the "payload" (bytes[4..7]).
    out.checksum = frame.data[8];

    return out;
}

uint8_t computeChecksum(const Frame72 &frame) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < 8; i++) {
        crc ^= frame.data[i];
    }
    return crc;
}

} // namespace ToshibaIR

#pragma once

// Sampling/session
#define READINGS_PER_SESSION 12
#define SAMPLE_INTERVAL_MS   5000  // 12 * 5s ≈ 60s

// Fever thresholds (Celsius)
#define THRESH_NORMAL_MAX_C  37.5
#define THRESH_FEVER_MAX_C   38.9  // High fever at >= 38.9

// I2C addresses
#define TMP117_ADDR    0x48
#define ST25DV_I2C_ADDR 0x53

// NDEF
#define NDEF_LANG_CODE "en"

// NFC text buffer size
#define NFC_TEXT_MAX 192

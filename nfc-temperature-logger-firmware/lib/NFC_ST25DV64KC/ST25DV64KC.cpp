#include "ST25DV64KC.h"

// Write len bytes starting at 16-bit memory address, page by page.
bool ST25DV64KC::writeBytes(uint16_t memAddr, const uint8_t* data, size_t len) {
  const size_t page = 16; // typical page size; adjust if needed
  size_t written = 0;

  while (written < len) {
    size_t chunk = min(page - (memAddr % page), len - written);
    _wire->beginTransmission(_addr);
    _wire->write((uint8_t)(memAddr >> 8));
    _wire->write((uint8_t)(memAddr & 0xFF));
    _wire->write(data + written, chunk);
    if (_wire->endTransmission() != 0) return false;
    delay(6); // tWR
    memAddr += chunk;
    written += chunk;
  }
  return true;
}

bool ST25DV64KC::begin() {
  // Basic probe by reading 1 byte
  _wire->beginTransmission(_addr);
  _wire->write((uint8_t)0x00);
  _wire->write((uint8_t)0x00);
  if (_wire->endTransmission(false) != 0) return false;
  _wire->requestFrom((int)_addr, 1);
  if (_wire->available()) { (void)_wire->read(); return true; }
  return false;
}

// Build a minimal NDEF message containing a single Text record.
size_t ST25DV64KC::buildTextRecord(uint8_t* buf, size_t maxLen, const char* lang, const char* text) {
  // NDEF TLV: | 0x03 (NDEF) | L | NDEF message bytes ... | 0xFE
  // NDEF message: single short record, TNF=Well-known, Type="T" (Text)
  const uint8_t tnf = 0x01; // well-known
  const char* type = "T";
  const uint8_t typeLen = 1;

  const uint8_t langLen = (uint8_t)min<size_t>(255, strlen(lang));
  const size_t textLen = min<size_t>(maxLen, strlen(text));
  const size_t payloadLen = 1 + langLen + textLen;

  // Short Record header (SR=1)
  const uint8_t header = 0xD1; // MB=1 ME=1 SR=1 TNF=1
  const uint8_t payloadLen8 = (uint8_t)payloadLen;

  // NDEF message length = header(1)+typeLen(1)+payloadLen(1)+type(1)+payload(1+langLen+textLen)
  const size_t ndefMsgLen = 1 + 1 + 1 + typeLen + payloadLen;

  // TLV length fits in 1 byte for this small message
  const size_t tlvLen = ndefMsgLen;

  size_t idx = 0;
  buf[idx++] = 0x03;            // NDEF TLV
  buf[idx++] = (uint8_t)tlvLen; // Length
  buf[idx++] = header;          // NDEF header
  buf[idx++] = typeLen;         // TYPE LEN
  buf[idx++] = payloadLen8;     // PAYLOAD LEN (short)
  buf[idx++] = (uint8_t)type[0];// Type 'T'

  // Payload: |status|lang|text|
  // status: b7=0 (UTF-8), b5..0 = lang code length
  buf[idx++] = langLen & 0x3F;
  memcpy(buf + idx, lang, langLen); idx += langLen;
  memcpy(buf + idx, text, textLen); idx += textLen;

  buf[idx++] = 0xFE; // Terminator TLV
  return idx;
}

bool ST25DV64KC::writeTextNDEF(const char* text, const char* lang) {
  uint8_t temp[256];
  size_t n = buildTextRecord(temp, sizeof(temp) - 1, lang, text);
  return writeBytes(NDEF_START, temp, n);
}

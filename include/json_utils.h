#ifndef JSON_UTILS_H
#define JSON_UTILS_H

#include <Arduino.h>
#include <ctype.h>
#include <stdint.h>

inline bool parseJsonUint32Field(const String& body, const char* key, uint32_t* outValue) {
  String needle = "\"" + String(key) + "\"";
  int keyPos = body.indexOf(needle);
  if (keyPos < 0) {
    return false;
  }

  int colonPos = body.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) {
    return false;
  }

  int i = colonPos + 1;
  while (i < body.length() && isspace(static_cast<unsigned char>(body[i]))) {
    i++;
  }

  bool quoted = false;
  if (i < body.length() && body[i] == '"') {
    quoted = true;
    i++;
  }

  if (i >= body.length() || !isdigit(static_cast<unsigned char>(body[i]))) {
    return false;
  }

  uint64_t value = 0;
  while (i < body.length() && isdigit(static_cast<unsigned char>(body[i]))) {
    value = value * 10 + static_cast<uint64_t>(body[i] - '0');
    if (value > UINT32_MAX) {
      return false;
    }
    i++;
  }

  if (quoted) {
    if (i >= body.length() || body[i] != '"') {
      return false;
    }
  }

  *outValue = static_cast<uint32_t>(value);
  return true;
}

inline bool parseJsonBoolField(const String& body, const char* key, bool* outValue) {
  String needle = "\"" + String(key) + "\"";
  int keyPos = body.indexOf(needle);
  if (keyPos < 0) {
    return false;
  }

  int colonPos = body.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) {
    return false;
  }

  int i = colonPos + 1;
  while (i < body.length() && isspace(static_cast<unsigned char>(body[i]))) {
    i++;
  }

  bool quoted = false;
  if (i < body.length() && body[i] == '"') {
    quoted = true;
    i++;
  }

  if (i + 4 <= body.length() && body.substring(i, i + 4) == "true") {
    i += 4;
    if (quoted) {
      if (i >= body.length() || body[i] != '"') {
        return false;
      }
    } else if (i < body.length() && body[i] != ',' && body[i] != '}' &&
               !isspace(static_cast<unsigned char>(body[i]))) {
      return false;
    }
    *outValue = true;
    return true;
  }

  if (i + 5 <= body.length() && body.substring(i, i + 5) == "false") {
    i += 5;
    if (quoted) {
      if (i >= body.length() || body[i] != '"') {
        return false;
      }
    } else if (i < body.length() && body[i] != ',' && body[i] != '}' &&
               !isspace(static_cast<unsigned char>(body[i]))) {
      return false;
    }
    *outValue = false;
    return true;
  }

  return false;
}

inline bool parseJsonStringField(const String& body, const char* key, String* outValue) {
  String needle = "\"" + String(key) + "\"";
  int keyPos = body.indexOf(needle);
  if (keyPos < 0) {
    return false;
  }

  int colonPos = body.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) {
    return false;
  }

  int i = colonPos + 1;
  while (i < body.length() && isspace(static_cast<unsigned char>(body[i]))) {
    i++;
  }

  if (i >= body.length() || body[i] != '"') {
    return false;
  }
  i++;

  String value;
  while (i < body.length()) {
    char c = body[i++];
    if (c == '"') {
      *outValue = value;
      return true;
    }

    if (c != '\\') {
      value += c;
      continue;
    }

    if (i >= body.length()) {
      return false;
    }

    char escaped = body[i++];
    if (escaped == '"' || escaped == '\\' || escaped == '/') {
      value += escaped;
    } else if (escaped == 'b') {
      value += '\b';
    } else if (escaped == 'f') {
      value += '\f';
    } else if (escaped == 'n') {
      value += '\n';
    } else if (escaped == 'r') {
      value += '\r';
    } else if (escaped == 't') {
      value += '\t';
    } else {
      return false;
    }
  }

  return false;
}

inline String escapeJsonString(const String& input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
      continue;
    }
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    if (c == '\r') {
      out += "\\r";
      continue;
    }
    if (c == '\t') {
      out += "\\t";
      continue;
    }
    out += c;
  }
  return out;
}

#endif

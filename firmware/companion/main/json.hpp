#pragma once

#include <string_view>

#include "cJSON.h"

namespace json {

// The returned view borrows the document's string storage.
inline std::string_view string(const cJSON* object, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

inline double number(const cJSON* object, const char* key, double fallback = 0) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

}  // namespace json

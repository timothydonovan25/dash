#include "drivers/nvs_storage.hpp"

#include "nvs.h"
#include "nvs_flash.h"
#include <cstdio>

namespace drivers {
namespace {

// NVS keys are strings; ConfigItem keys are numbers that must stay stable
// forever. "k0101" is that number, printed. Never derive a key from an
// index or a label — both move, and a moved key is a setting the rider
// loses on the next flash.
void key_name(uint16_t key, char* out, size_t n) {
    std::snprintf(out, n, "k%04X", key);
}

hal::Status from_esp(esp_err_t err) {
    switch (err) {
        case ESP_OK: return hal::Status::Ok;
        case ESP_ERR_NVS_NOT_FOUND: return hal::Status::NotPresent;
        case ESP_ERR_NVS_INVALID_HANDLE: return hal::Status::NotPresent;
        default: return hal::Status::IoError;
    }
}

}  // namespace

hal::Status NvsStorage::init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // A partition-table change erases NVS, which takes the odometer with
        // it. §17 says set the table before step 2 for exactly this reason.
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ready_ = (err == ESP_OK);
    return from_esp(err);
}

hal::Status NvsStorage::read_f32(const char* ns, uint16_t key, float& out) {
    if (!ready_) return hal::Status::NotPresent;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return from_esp(err);

    char name[8];
    key_name(key, name, sizeof name);
    uint32_t raw = 0;
    err = nvs_get_u32(h, name, &raw);
    nvs_close(h);
    if (err != ESP_OK) return from_esp(err);      // NotPresent: keep the default

    // Stored as the bit pattern rather than as a blob: NVS has no float type
    // and this keeps one entry per key.
    static_assert(sizeof(float) == sizeof(uint32_t), "float is not 32 bits");
    __builtin_memcpy(&out, &raw, sizeof out);
    return hal::Status::Ok;
}

hal::Status NvsStorage::write_f32(const char* ns, uint16_t key, float value) {
    if (!ready_) return hal::Status::NotPresent;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return from_esp(err);

    char name[8];
    key_name(key, name, sizeof name);
    uint32_t raw = 0;
    __builtin_memcpy(&raw, &value, sizeof raw);
    err = nvs_set_u32(h, name, raw);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return from_esp(err);
}

hal::Status NvsStorage::read_f64(const char* ns, uint16_t key, double& out) {
    if (!ready_) return hal::Status::NotPresent;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return from_esp(err);

    char name[8];
    key_name(key, name, sizeof name);
    uint64_t raw = 0;
    err = nvs_get_u64(h, name, &raw);
    nvs_close(h);
    if (err != ESP_OK) return from_esp(err);
    __builtin_memcpy(&out, &raw, sizeof out);
    return hal::Status::Ok;
}

hal::Status NvsStorage::write_f64(const char* ns, uint16_t key, double value) {
    if (!ready_) return hal::Status::NotPresent;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return from_esp(err);

    char name[8];
    key_name(key, name, sizeof name);
    uint64_t raw = 0;
    __builtin_memcpy(&raw, &value, sizeof raw);
    err = nvs_set_u64(h, name, raw);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return from_esp(err);
}

hal::Status NvsStorage::commit() {
    if (!ready_) return hal::Status::NotPresent;
    // Each write above commits before it closes its own handle.
    // The odometer is written once a second at most by the keeper — NVS wear
    // levelling copes with that for the life of the bike, but writing it at
    // 50 Hz from the logic task would not, which is why the keeper owns it.
    return hal::Status::Ok;
}

}  // namespace drivers

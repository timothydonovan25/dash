#include "c6_firmware.hpp"

#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_ota_ops.h"

#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cinttypes>

namespace services {
namespace {

constexpr const char* kTag = "c6_fw";
constexpr const char* kPartitionLabel = "slave_fw";
constexpr size_t      kChunkSize = 1500;   // matches the SDIO link's own frame size

// The C6 build this ships next to (demo_p4/c6_cp, flashed into slave_fw by
// main/CMakeLists.txt) is Espressif's "2.12.8" ESP-Hosted slave; anything
// older than that is what this host was not built to talk to (see the
// "Version mismatch" warning the transport layer logs on its own).
constexpr uint32_t kNeedMajor = 2;
constexpr uint32_t kNeedMinor = 12;

bool version_needs_upgrade(const esp_hosted_coprocessor_fwver_t& v) {
    return v.major1 < kNeedMajor || (v.major1 == kNeedMajor && v.minor1 < kNeedMinor);
}

// The C6 is shared by both firmware slots (§11.7). A dev image runs once and
// is gone at the next reset, but a C6 it upgraded stays upgraded, and stable
// then talks to it. Within a major version ESP-Hosted keeps compatibility;
// across one it does not promise to. So dev may not cross a major version.
bool running_dev() {
    esp_ota_img_states_t s;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) == ESP_OK &&
           s == ESP_OTA_IMG_PENDING_VERIFY;
}

// Only cares about the total on-flash image size (header + segments +
// checksum + optional SHA256), so a partition that's larger than the
// actual image doesn't send trailing 0xFF padding as if it were firmware.
bool image_size(const esp_partition_t* part, size_t* out_size) {
    esp_image_header_t header;
    if (esp_partition_read(part, 0, &header, sizeof(header)) != ESP_OK) return false;
    if (header.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(kTag, "partition '%s' has no valid image (magic=0x%02x)",
                 part->label, header.magic);
        return false;
    }

    size_t offset = sizeof(header);
    size_t total  = sizeof(header);
    for (int i = 0; i < header.segment_count; ++i) {
        esp_image_segment_header_t seg;
        if (esp_partition_read(part, offset, &seg, sizeof(seg)) != ESP_OK) return false;
        total  += sizeof(seg) + seg.data_len;
        offset += sizeof(seg) + seg.data_len;
    }

    total += (16 - (total % 16)) % 16;   // pad to 16
    total += 1;                          // checksum byte
    if (header.hash_appended) {
        total += (16 - (total % 16)) % 16;
        total += 32;                     // SHA256
    }
    *out_size = total;
    return true;
}

bool push_partition_to_slave(const esp_partition_t* part, size_t fw_size) {
    if (esp_hosted_slave_ota_begin() != ESP_OK) {
        ESP_LOGE(kTag, "ota_begin failed");
        return false;
    }

    uint8_t chunk[kChunkSize];
    size_t offset = 0;
    while (offset < fw_size) {
        const size_t n = (fw_size - offset > kChunkSize) ? kChunkSize : (fw_size - offset);
        if (esp_partition_read(part, offset, chunk, n) != ESP_OK ||
            esp_hosted_slave_ota_write(chunk, static_cast<uint32_t>(n)) != ESP_OK) {
            ESP_LOGE(kTag, "ota_write failed at offset %u", static_cast<unsigned>(offset));
            esp_hosted_slave_ota_end();
            return false;
        }
        offset += n;
    }

    if (esp_hosted_slave_ota_end() != ESP_OK) {
        ESP_LOGE(kTag, "ota_end failed");
        return false;
    }
    ESP_LOGI(kTag, "sent %u bytes to the C6", static_cast<unsigned>(fw_size));
    return true;
}

}  // namespace

void c6_firmware_update_if_needed() {
    esp_hosted_coprocessor_fwver_t fwver;
    if (esp_hosted_get_coprocessor_fwversion(&fwver) != ESP_OK) {
        ESP_LOGW(kTag, "could not read C6 firmware version, skipping OTA check");
        return;
    }
    if (!version_needs_upgrade(fwver)) {
        ESP_LOGI(kTag, "C6 firmware %" PRIu32 ".%" PRIu32 ".%" PRIu32 " already current",
                 fwver.major1, fwver.minor1, fwver.patch1);
        return;
    }

    if (fwver.major1 != kNeedMajor && running_dev()) {
        ESP_LOGW(kTag, "dev firmware will not move the C6 from %" PRIu32 ".x to %" PRIu32
                 ".x: stable shares it. Promote first, or flash over USB.",
                 fwver.major1, kNeedMajor);
        return;
    }

    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), kPartitionLabel);
    if (part == nullptr) {
        ESP_LOGE(kTag, "no '%s' partition — can't upgrade the C6 over SDIO", kPartitionLabel);
        return;
    }

    size_t fw_size = 0;
    if (!image_size(part, &fw_size) || fw_size == 0 || fw_size > part->size) {
        ESP_LOGE(kTag, "'%s' partition has no usable C6 firmware image", kPartitionLabel);
        return;
    }

    ESP_LOGW(kTag, "C6 firmware %" PRIu32 ".%" PRIu32 ".%" PRIu32 " is older than this host "
             "expects — pushing %u bytes over SDIO",
             fwver.major1, fwver.minor1, fwver.patch1, static_cast<unsigned>(fw_size));
    if (!push_partition_to_slave(part, fw_size)) return;

    if (esp_hosted_slave_ota_activate() != ESP_OK) {
        ESP_LOGE(kTag, "ota_activate failed — C6 still has the old firmware");
        return;
    }
    ESP_LOGW(kTag, "C6 activated new firmware — restarting host to resync");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

}  // namespace services

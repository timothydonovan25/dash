#include "c6_firmware.hpp"
#include "core/c6/c6.hpp"
#include "esp_app_desc.h"
#include "nvs.h"

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

// Which C6 build was last pushed, so an image the C6 will not take is tried
// once rather than on every boot (core::c6::decide).
constexpr const char* kNvsNs = "c6fw";
constexpr const char* kNvsTried = "tried";

int32_t tried_build() {
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, kNvsTried, &v);
        nvs_close(h);
    }
    return v;
}

void remember_tried(int32_t build) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, kNvsTried, build);
    nvs_commit(h);
    nvs_close(h);
}

// The image in slave_fw, identified the way the C6 identifies itself: its
// version string and the first four bytes of its ELF SHA-256, both from the
// app description that follows the first segment header.
core::c6::FwId image_id(const esp_partition_t* part) {
    core::c6::FwId id;
    esp_app_desc_t desc;
    constexpr size_t kDescOffset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (!part || esp_partition_read(part, kDescOffset, &desc, sizeof desc) != ESP_OK ||
        desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return id;
    }
    desc.version[sizeof desc.version - 1] = '\0';
    if (!core::c6::parse_version(desc.version, id)) return id;
    id.build = core::c6::build_id(desc.app_elf_sha256);
    id.valid = true;
    return id;
}

const esp_partition_t* slave_partition() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    static_cast<esp_partition_subtype_t>(0x40), kPartitionLabel);
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

    // Static: 1.5 kB was most of the calling task's stack, and there is only
    // ever one push, at boot.
    static uint8_t chunk[kChunkSize];
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

// slave_fw to the C6, then a restart so both ends start over in step.
// Returns only if something failed, having said what.
void push_and_restart() {
    const esp_partition_t* part = slave_partition();
    if (part == nullptr) {
        ESP_LOGE(kTag, "no '%s' partition — can't upgrade the C6 over SDIO", kPartitionLabel);
        return;
    }

    size_t fw_size = 0;
    if (!image_size(part, &fw_size) || fw_size == 0 || fw_size > part->size) {
        ESP_LOGE(kTag, "'%s' partition has no usable C6 firmware image", kPartitionLabel);
        return;
    }

    ESP_LOGW(kTag, "pushing %u bytes over SDIO", static_cast<unsigned>(fw_size));
    if (!push_partition_to_slave(part, fw_size)) return;

    if (esp_hosted_slave_ota_activate() != ESP_OK) {
        ESP_LOGE(kTag, "ota_activate failed — C6 still has the old firmware");
        return;
    }
    ESP_LOGW(kTag, "C6 activated new firmware — restarting host to resync");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

}  // namespace

// The C6 runs exactly the image in slave_fw (core::c6). Called once the
// SDIO link is up; restarts the dash if it pushed.
void c6_firmware_update_if_needed() {
    esp_hosted_coprocessor_fwver_t v;
    core::c6::FwId running;
    if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK) {
        running.major = v.major1;
        running.minor = v.minor1;
        running.patch = v.patch1;
        running.build = v.build > 0 ? v.build : 0;
        running.valid = true;
    } else {
        ESP_LOGW(kTag, "could not read the C6's firmware version, leaving it");
    }
    const core::c6::FwId image = image_id(slave_partition());
    if (!image.valid) {
        ESP_LOGW(kTag, "no C6 image in %s to compare against", kPartitionLabel);
    }
    const int32_t tried = tried_build();
    const core::c6::Action a = core::c6::decide(running, image, tried, running_dev());
    ESP_LOGI(kTag, "C6 runs %" PRIu32 ".%" PRIu32 ".%" PRIu32 " #%08" PRIx32
             ", %s has %" PRIu32 ".%" PRIu32 ".%" PRIu32 " #%08" PRIx32 ": %s",
             running.major, running.minor, running.patch, static_cast<uint32_t>(running.build),
             kPartitionLabel, image.major, image.minor, image.patch,
             static_cast<uint32_t>(image.build), core::c6::action_name(a));
    if (a != core::c6::Action::Push) return;
    // Recorded before the push, so a push that wedges the C6 or the link
    // is not retried at every boot.
    remember_tried(image.build);
    push_and_restart();
}

void c6_firmware_push_now() {
    ESP_LOGW(kTag, "replacing the C6 firmware from %s", kPartitionLabel);
    push_and_restart();
}

}  // namespace services

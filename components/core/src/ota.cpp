#include "core/ota/ota.hpp"

#include <cstring>

namespace core::ota {
namespace {

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// A fixed-width C string field from the image, terminated whether or not
// the image terminated it.
void copy_field(char* dst, size_t cap, const uint8_t* src, size_t len) {
    size_t n = 0;
    while (n < len && n + 1 < cap && src[n] != '\0') {
        dst[n] = static_cast<char>(src[n]);
        ++n;
    }
    dst[n] = '\0';
}

}  // namespace

const char* image_error_name(ImageError e) {
    switch (e) {
        case ImageError::None:         return "ok";
        case ImageError::TooShort:     return "too short";
        case ImageError::BadMagic:     return "not an image";
        case ImageError::WrongChip:    return "wrong chip";
        case ImageError::NoAppDesc:    return "not an app";
        case ImageError::WrongProject: return "not dash";
        case ImageError::TooBig:       return "too big";
        case ImageError::WrongBoard:   return "other board's image";
    }
    return "?";
}

ImageError check_header(const uint8_t* head, size_t len, size_t file_size,
                        size_t slot_size, ImageInfo& out) {
    out = ImageInfo{};
    if (!head || len < kHeaderBytes || file_size < kHeaderBytes) {
        return ImageError::TooShort;
    }
    if (head[0] != kImageMagic || head[1] == 0) return ImageError::BadMagic;
    if (le16(head + kChipIdOffset) != kChipEsp32P4) return ImageError::WrongChip;
    if (le32(head + kAppDescOffset) != kAppDescMagic) return ImageError::NoAppDesc;

    char project[32];
    copy_field(project, sizeof project, head + kProjectOffset, 32);
    if (std::strcmp(project, kProjectName) != 0) return ImageError::WrongProject;
    if (file_size > slot_size) return ImageError::TooBig;

    copy_field(out.version, sizeof out.version, head + kVersionOffset, kVersionLen);
    char date[kDateLen + 1];
    copy_field(date, sizeof date, head + kDateOffset, kDateLen);
    iso_date(date, out.date, sizeof out.date);
    return ImageError::None;
}

void iso_date(const char* d, char* out, size_t n) {
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!d || n < 11 || std::strlen(d) < 11) return;
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month = 0;
    for (int i = 0; i < 12; ++i) {
        if (std::strncmp(d, kMonths[i], 3) == 0) { month = i + 1; break; }
    }
    // "Mmm dd yyyy", with the day space-padded.
    const char d1 = d[4] == ' ' ? '0' : d[4];
    const char d2 = d[5];
    const bool digits = d1 >= '0' && d1 <= '3' && d2 >= '0' && d2 <= '9' && d[3] == ' ' &&
                        d[6] == ' ';
    for (int i = 7; i < 11 && digits; ++i) {
        if (d[i] < '0' || d[i] > '9') return;
    }
    if (month == 0 || !digits) return;
    out[0] = d[7]; out[1] = d[8]; out[2] = d[9]; out[3] = d[10];
    out[4] = '-';
    out[5] = static_cast<char>('0' + month / 10);
    out[6] = static_cast<char>('0' + month % 10);
    out[7] = '-';
    out[8] = d1; out[9] = d2;
    out[10] = '\0';
}

bool board_of(const char* v, char* out, size_t n) {
    if (!v || !out || n == 0) return false;
    out[0] = '\0';
    // "<digits>.<digits>[.<digits>] <board> <feature...>"
    const char* p = v;
    int dots = 0;
    bool digit = false;
    for (; *p && *p != ' '; ++p) {
        if (*p == '.') {
            if (!digit) return false;
            ++dots;
            digit = false;
        } else if (*p >= '0' && *p <= '9') {
            digit = true;
        } else {
            return false;
        }
    }
    if (*p != ' ' || dots < 1 || dots > 2 || !digit) return false;
    ++p;
    size_t k = 0;
    for (; *p && *p != ' '; ++p) {
        if (k + 1 >= n) return false;
        out[k++] = *p;
    }
    out[k] = '\0';
    // A board needs a feature after it.
    return k > 0 && *p == ' ' && p[1] != '\0';
}

void bin_name(const char* version, const char* iso_date, char* out, size_t n) {
    if (!out || n == 0) return;
    out[0] = '\0';
    if (n < 10) return;                              // "dash-.bin" and a NUL
    const size_t room = n - 5;                        // keep ".bin" + NUL
    size_t k = 0;
    auto put = [&](char c) { if (k < room) out[k++] = c; };
    auto put_s = [&](const char* s) { for (; s && *s; ++s) put(*s); };

    // Words, lowercased, with only letters and digits kept inside a word:
    // "Wi-Fi OTA" -> "wifi-ota".
    auto put_words = [&](const char* s) {
        bool gap = false, any = false;
        for (; s && *s; ++s) {
            char c = *s;
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c == ' ') { gap = any; continue; }
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.')) continue;
            if (gap) put('-');
            gap = false;
            any = true;
            put(c);
        }
    };

    put_s("dash-");
    char board[16];
    if (board_of(version, board, sizeof board)) {
        const char* v = version;
        while (*v != ' ') put(*v++);                  // the number
        if (iso_date && *iso_date) { put('-'); put_s(iso_date); }
        put('-');
        put_s(board);
        const char* feature = std::strchr(v + 1, ' ') + 1;
        put('-');
        put_words(feature);
    } else {
        put_words(version && *version ? version : "unknown");
        if (iso_date && *iso_date) { put('-'); put_s(iso_date); }
    }
    out[k] = '\0';
    std::strcat(out, ".bin");
}

ImageError check_board(const char* running, const char* image) {
    char a[16], b[16];
    if (!board_of(running, a, sizeof a) || !board_of(image, b, sizeof b)) {
        return ImageError::None;
    }
    return std::strcmp(a, b) == 0 ? ImageError::None : ImageError::WrongBoard;
}

const char* role_name(Role r) {
    switch (r) {
        case Role::Stable: return "stable";
        case Role::Dev:    return "dev";
    }
    return "?";
}

Role role_of(SlotState running) {
    return running == SlotState::PendingVerify || running == SlotState::New ? Role::Dev
                                                                             : Role::Stable;
}

Other other_from(bool has_valid_image, SlotState state) {
    if (state == SlotState::Invalid) return Other::Invalid;
    return has_valid_image ? Other::Ready : Other::Empty;
}

// --- installing ----------------------------------------------------------

const char* install_error_name(InstallError e) {
    switch (e) {
        case InstallError::None:         return "ok";
        case InstallError::Moving:       return "park first";
        case InstallError::NotStable:    return "run stable first";
        case InstallError::NotDev:       return "run dev first";
        case InstallError::NoCard:       return "no card";
        case InstallError::NoFile:       return "no update/dash.bin";
        case InstallError::BadImage:     return "bad image";
        case InstallError::ReadFailed:   return "read failed";
        case InstallError::FlashFailed:  return "flash write failed";
        case InstallError::VerifyFailed: return "image check failed";
        case InstallError::NoDev:        return "no dev image";
    }
    return "?";
}

bool may_write(float speed_mph, float rpm) {
    return speed_mph < 0.5f && rpm < 1.0f;
}

uint8_t percent(size_t done, size_t total) {
    if (total == 0) return 0;
    if (done >= total) return 100;
    return static_cast<uint8_t>((static_cast<uint64_t>(done) * 100u) / total);
}

}  // namespace core::ota

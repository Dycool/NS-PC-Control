#include "amiibo_library.hpp"
#include "s2_nfc_codec.hpp"

// Key derivation/tag packing is based on amiitool:
// Copyright (c) 2015-2017 Marcos Del Sol Vives
// Copyright (c) 2016 javiMaD
// Copyright (c) 2020-2021 nitz / chris marc dailey
// Used under the MIT License; see docs/third-party/amiitool-MIT.txt.

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>

#ifdef NS_EMBEDDED_AMIIBO_TEMPLATES
#include "amiibo_templates_embed.h"
#endif

namespace amiibo_library {
namespace {

// The tag construction below is a portable OpenSSL port of the MIT-licensed
// amiitool code used by Pixl.js. No retail keys or Nintendo tag dumps are
// included in NS-PC-Control.
constexpr std::size_t MASTER_KEY_SIZE = 80;
constexpr std::size_t INTERNAL_SIZE = 520;
constexpr std::size_t RAW_SIZE = 540;
constexpr std::size_t V3_SIZE = 2048;
constexpr std::size_t CIPHER_OFFSET = 0x02c;
constexpr std::size_t CIPHER_LENGTH = 0x188;
constexpr std::size_t DATA_HMAC_POS = 0x008;
constexpr std::size_t TAG_HMAC_POS = 0x1b4;
constexpr uint32_t FORMAT_REQUEST_FLAG = 0x80000000u;

std::mutex g_mutex;
std::array<std::string, 4> g_selected_ids;
thread_local std::optional<ns::s2nfc::Signature> g_pending_v3_read_prefix;

bool consume_pending_v3_read_prefix(ns::s2nfc::Signature& output) {
    if (!g_pending_v3_read_prefix) return false;
    output = *g_pending_v3_read_prefix;
    g_pending_v3_read_prefix.reset();
    return true;
}

struct V3ReadPrefixResolverRegistration {
    V3ReadPrefixResolverRegistration() {
        ns::s2nfc::set_v3_read_prefix_resolver(&consume_pending_v3_read_prefix);
    }
} g_v3_read_prefix_resolver_registration;

std::filesystem::path library_root() {
    if (const char* configured = std::getenv("NS_PC_CONTROL_DATA_DIR");
            configured && *configured) {
        return std::filesystem::path(configured) / "amiibo";
    }
    return "/var/lib/ns-pc-control/amiibo";
}

std::string tag_id(uint32_t head, uint32_t tail) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(8) << head
        << std::setw(8) << tail;
    return out.str();
}

std::filesystem::path tag_path(const std::string& id) {
    return library_root() / "tags" / (id + ".bin");
}

bool validate_key(std::span<const uint8_t> key) {
    if (key.size() != RETAIL_KEY_SIZE) return false;
    // magicBytesSize is byte 31 in each packed 80-byte master-key record.
    return key[31] <= 16 && key[MASTER_KEY_SIZE + 31] <= 16;
}

bool ensure_private_directory(const std::filesystem::path& path,
                              std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        error = "cannot create " + path.string() + ": " + ec.message();
        return false;
    }
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        ec);
    if (ec) {
        error = "cannot restrict " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool atomic_write(const std::filesystem::path& path,
                  std::span<const uint8_t> data,
                  std::string& error) {
    if (!ensure_private_directory(path.parent_path(), error)) return false;
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot open " + temp.string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out) {
            error = "cannot write " + temp.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::permissions(
        temp,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        ec);
    if (ec) {
        error = "cannot restrict " + temp.string() + ": " + ec.message();
        std::filesystem::remove(temp);
        return false;
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temp, path, ec);
    }
    if (ec) {
        error = "cannot replace " + path.string() + ": " + ec.message();
        std::filesystem::remove(temp);
        return false;
    }
    return true;
}

std::optional<std::vector<uint8_t>> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0) return std::nullopt;
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<std::size_t>(size));
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    if (!in) return std::nullopt;
    return data;
}

std::optional<std::array<uint8_t, RETAIL_KEY_SIZE>> load_retail_key(
    std::string& error) {
    std::vector<std::filesystem::path> candidates;
    if (const char* configured = std::getenv("NS_AMIIBO_RETAIL_KEY");
            configured && *configured) {
        candidates.emplace_back(configured);
    }
    candidates.push_back(library_root() / "key_retail.bin");
    for (const auto& path : candidates) {
        const auto bytes = read_file(path);
        if (!bytes) continue;
        if (!validate_key(*bytes)) {
            error = path.string() + " is not a valid 160-byte key_retail.bin";
            return std::nullopt;
        }
        std::array<uint8_t, RETAIL_KEY_SIZE> key{};
        std::copy_n(bytes->begin(), key.size(), key.begin());
        return key;
    }
    error = "Format Amiibo requires a valid 160-byte key_retail.bin at "
        + (library_root() / "key_retail.bin").string()
        + " or NS_AMIIBO_RETAIL_KEY";
    return std::nullopt;
}

std::array<uint8_t, 32> hmac_sha256(std::span<const uint8_t> key,
                                    std::span<const uint8_t> data) {
    std::array<uint8_t, 32> result{};
    unsigned int size = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         data.data(), data.size(), result.data(), &size);
    return result;
}

std::vector<uint8_t> prepare_seed(std::span<const uint8_t, MASTER_KEY_SIZE> master,
                                  std::span<const uint8_t, 64> base_seed) {
    std::vector<uint8_t> seed;
    seed.reserve(79);
    bool terminated = false;
    for (std::size_t i = 0; i < 14; ++i) {
        seed.push_back(master[16 + i]);
        if (master[16 + i] == 0) {
            terminated = true;
            break;
        }
    }
    (void)terminated; // The retail records contain a terminated type string.
    const std::size_t magic_size = master[31];
    seed.insert(seed.end(), base_seed.begin(),
                base_seed.begin() + static_cast<std::ptrdiff_t>(16 - magic_size));
    seed.insert(seed.end(), master.begin() + 32,
                master.begin() + 32 + static_cast<std::ptrdiff_t>(magic_size));
    seed.insert(seed.end(), base_seed.begin() + 16, base_seed.begin() + 32);
    for (std::size_t i = 0; i < 32; ++i) {
        seed.push_back(base_seed[32 + i] ^ master[48 + i]);
    }
    return seed;
}

std::array<uint8_t, 48> derive_keys(
    std::span<const uint8_t, MASTER_KEY_SIZE> master,
    std::span<const uint8_t, 64> base_seed) {
    const std::vector<uint8_t> prepared = prepare_seed(master, base_seed);
    std::array<uint8_t, 48> derived{};
    std::size_t written = 0;
    uint16_t counter = 0;
    while (written < derived.size()) {
        std::vector<uint8_t> input;
        input.reserve(2 + prepared.size());
        input.push_back(static_cast<uint8_t>(counter >> 8));
        input.push_back(static_cast<uint8_t>(counter));
        input.insert(input.end(), prepared.begin(), prepared.end());
        ++counter;
        const auto block = hmac_sha256(master.first<16>(), input);
        const std::size_t take = std::min(block.size(), derived.size() - written);
        std::copy_n(block.begin(), take, derived.begin() + written);
        written += take;
    }
    return derived;
}

std::array<uint8_t, 64> calculate_seed(std::span<const uint8_t> internal) {
    std::array<uint8_t, 64> seed{};
    std::copy_n(internal.begin() + 0x029, 2, seed.begin());
    std::copy_n(internal.begin() + 0x1d4, 8, seed.begin() + 0x10);
    std::copy_n(internal.begin() + 0x1d4, 8, seed.begin() + 0x18);
    std::copy_n(internal.begin() + 0x1e8, 32, seed.begin() + 0x20);
    return seed;
}

bool aes_ctr(std::span<const uint8_t, 16> key,
             std::span<const uint8_t, 16> iv,
             std::span<const uint8_t> input,
             std::span<uint8_t> output) {
    if (input.size() != output.size()) return false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int produced = 0;
    int final_size = 0;
    const bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr,
                                       key.data(), iv.data()) == 1
        && EVP_EncryptUpdate(ctx, output.data(), &produced, input.data(),
                             static_cast<int>(input.size())) == 1
        && EVP_EncryptFinal_ex(ctx, output.data() + produced, &final_size) == 1
        && static_cast<std::size_t>(produced + final_size) == input.size();
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

void internal_to_tag(std::span<const uint8_t> internal,
                     std::span<uint8_t> tag, bool v3) {
    std::copy_n(internal.begin() + 0x000, 0x008, tag.begin() + 0x008);
    std::copy_n(internal.begin() + 0x008, 0x020,
                tag.begin() + (v3 ? 0x0c0 : 0x080));
    std::copy_n(internal.begin() + 0x028, 0x024, tag.begin() + 0x010);
    std::copy_n(internal.begin() + 0x04c, 0x168,
                tag.begin() + (v3 ? 0x0e0 : 0x0a0));
    std::copy_n(internal.begin() + 0x1b4, 0x020, tag.begin() + 0x034);
    std::copy_n(internal.begin() + 0x1d4, 0x008, tag.begin() + 0x000);
    std::copy_n(internal.begin() + 0x1dc, 0x02c, tag.begin() + 0x054);
}

bool pack_tag(std::span<const uint8_t, RETAIL_KEY_SIZE> retail_key,
              std::span<const uint8_t> plain,
              std::span<uint8_t> tag, bool v3) {
    const auto seed = calculate_seed(plain);
    const auto tag_keys = derive_keys(
        std::span<const uint8_t, MASTER_KEY_SIZE>(retail_key.data() + MASTER_KEY_SIZE,
                                                  MASTER_KEY_SIZE),
        seed);
    const auto data_keys = derive_keys(
        std::span<const uint8_t, MASTER_KEY_SIZE>(retail_key.data(), MASTER_KEY_SIZE),
        seed);

    std::array<uint8_t, INTERNAL_SIZE> cipher{};
    const auto tag_hmac = hmac_sha256(
        std::span<const uint8_t>(tag_keys.data() + 32, 16),
        plain.subspan(0x1d4, 0x34));
    std::copy(tag_hmac.begin(), tag_hmac.end(), cipher.begin() + TAG_HMAC_POS);

    std::vector<uint8_t> data_hmac_input;
    data_hmac_input.reserve(0x18b + 0x20 + 0x34);
    data_hmac_input.insert(data_hmac_input.end(),
                           plain.begin() + 0x029, plain.begin() + 0x1b4);
    data_hmac_input.insert(data_hmac_input.end(),
                           tag_hmac.begin(), tag_hmac.end());
    data_hmac_input.insert(data_hmac_input.end(),
                           plain.begin() + 0x1d4, plain.begin() + 0x208);
    const auto data_hmac = hmac_sha256(
        std::span<const uint8_t>(data_keys.data() + 32, 16),
        data_hmac_input);
    std::copy(data_hmac.begin(), data_hmac.end(), cipher.begin() + DATA_HMAC_POS);

    if (!aes_ctr(
            std::span<const uint8_t, 16>(data_keys.data(), 16),
            std::span<const uint8_t, 16>(data_keys.data() + 16, 16),
            plain.subspan(CIPHER_OFFSET, CIPHER_LENGTH),
            std::span<uint8_t>(cipher).subspan(CIPHER_OFFSET, CIPHER_LENGTH))) {
        return false;
    }
    std::copy_n(plain.begin(), 8, cipher.begin());
    std::copy_n(plain.begin() + 0x028, 4, cipher.begin() + 0x028);
    std::copy_n(plain.begin() + 0x1d4, 0x34, cipher.begin() + 0x1d4);
    internal_to_tag(cipher, tag, v3);
    return true;
}

void write_be32(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value >> 24);
    dst[1] = static_cast<uint8_t>(value >> 16);
    dst[2] = static_cast<uint8_t>(value >> 8);
    dst[3] = static_cast<uint8_t>(value);
}

uint16_t crc16_mcrf4xx(std::span<const uint8_t> data) {
    uint16_t crc = 0xffff;
    for (const uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408)
                            : static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

std::optional<std::vector<uint8_t>> generate_tag(
    uint32_t head, uint32_t tail,
    std::span<const uint8_t, RETAIL_KEY_SIZE> retail_key) {
    std::array<uint8_t, RAW_SIZE> plain{};
    std::array<uint8_t, 7> uid{};
    uid[0] = 0x04;
    if (RAND_bytes(uid.data() + 1, 6) != 1
            || RAND_bytes(plain.data() + 0x1e8, 32) != 1) {
        return std::nullopt;
    }

    write_be32(plain.data() + 0x1dc, head);
    write_be32(plain.data() + 0x1e0, tail);
    write_be32(plain.data() + 0x054, head);
    write_be32(plain.data() + 0x058, tail);

    constexpr std::array<uint8_t, 8> internal_static_lock{
        0x65, 0x48, 0x0f, 0xe0, 0xf1, 0x10, 0xff, 0xee};
    constexpr std::array<uint8_t, 4> a5_static{0xa5, 0x00, 0x00, 0x00};
    std::copy(internal_static_lock.begin(), internal_static_lock.end(), plain.begin());
    plain[0] = uid[3] ^ uid[4] ^ uid[5] ^ uid[6];
    plain[0x1d4] = uid[0];
    plain[0x1d5] = uid[1];
    plain[0x1d6] = uid[2];
    plain[0x1d7] = 0x88 ^ uid[0] ^ uid[1] ^ uid[2];
    std::copy(uid.begin() + 3, uid.end(), plain.begin() + 0x1d8);
    std::copy(a5_static.begin(), a5_static.end(), plain.begin() + 0x028);

    const bool v3 = (tail & 0xffu) == 0x03u;
    std::vector<uint8_t> tag(v3 ? V3_SIZE : RAW_SIZE);
    if (!pack_tag(retail_key, plain, tag, v3)) return std::nullopt;
    constexpr std::array<uint8_t, 20> lock{
        0x01, 0x00, 0x0f, 0xbd, 0x00, 0x00, 0x00, 0x04, 0x5f, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::copy(lock.begin(), lock.end(), tag.begin() + (v3 ? 0x248 : INTERNAL_SIZE));

    if (!v3) return tag;

    std::fill(tag.begin() + 0x25c, tag.begin() + 0x388, 0);
    constexpr std::array<uint8_t, 4> page_e2{0x01, 0x00, 0xff, 0x00};
    constexpr std::array<uint8_t, 4> page_e3{0x00, 0x00, 0x00, 0x04};
    constexpr std::array<uint8_t, 4> page_e4{0x07, 0x00, 0x00, 0x00};
    constexpr std::array<uint8_t, 4> page_ec{0x41, 0x00, 0xf8, 0x48};
    constexpr std::array<uint8_t, 4> page_ed{0x08, 0x01, 0x29, 0x00};
    std::copy(page_e2.begin(), page_e2.end(), tag.begin() + 0x388);
    std::copy(page_e3.begin(), page_e3.end(), tag.begin() + 0x38c);
    std::copy(page_e4.begin(), page_e4.end(), tag.begin() + 0x390);
    std::fill(tag.begin() + 0x394, tag.begin() + 0x3b0, 0);
    std::copy(page_ec.begin(), page_ec.end(), tag.begin() + 0x3b0);
    std::copy(page_ed.begin(), page_ed.end(), tag.begin() + 0x3b4);
    std::fill(tag.begin() + 0x3b8, tag.begin() + 0x400, 0);
    const uint16_t crc = crc16_mcrf4xx(
        std::span<const uint8_t>(tag.data() + 0x3c0, 0x3e));
    tag[0x3fe] = static_cast<uint8_t>(crc >> 8);
    tag[0x3ff] = static_cast<uint8_t>(crc);

    std::copy_n(uid.begin(), 7, tag.begin());
    tag[7] = 0x00;
    tag[8] = 0x44;
    return tag;
}

#ifdef NS_EMBEDDED_AMIIBO_TEMPLATES
uint32_t read_le32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0])
        | (static_cast<uint32_t>(data[1]) << 8)
        | (static_cast<uint32_t>(data[2]) << 16)
        | (static_cast<uint32_t>(data[3]) << 24);
}
#endif

std::optional<std::vector<uint8_t>> embedded_template(
    uint32_t head, uint32_t tail) {
#ifdef NS_EMBEDDED_AMIIBO_TEMPLATES
    const std::span<const uint8_t> bundle(
        amiibo_templates_bin, amiibo_templates_bin_len);
    constexpr std::size_t header_size = 12;
    constexpr std::size_t entry_size = 16;
    if (bundle.size() < header_size
            || std::memcmp(bundle.data(), "NSAT", 4) != 0
            || bundle[4] != 1 || bundle[5] != 0
            || bundle[6] != entry_size || bundle[7] != 0) {
        return std::nullopt;
    }
    const uint32_t count = read_le32(bundle.data() + 8);
    if (count > (bundle.size() - header_size) / entry_size) {
        return std::nullopt;
    }
    for (uint32_t index = 0; index < count; ++index) {
        const uint8_t* entry =
            bundle.data() + header_size + index * entry_size;
        if (read_le32(entry) != head || read_le32(entry + 4) != tail) continue;
        const uint32_t offset = read_le32(entry + 8);
        const uint32_t length = read_le32(entry + 12);
        if (!ns::is_supported_amiibo_dump_size(length)
                || offset > bundle.size()
                || length > bundle.size() - offset) {
            return std::nullopt;
        }
        return std::vector<uint8_t>(
            bundle.begin() + offset, bundle.begin() + offset + length);
    }
#else
    (void)head;
    (void)tail;
#endif
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> factory_template(
    uint32_t head, uint32_t tail, std::span<const uint8_t> fallback_template) {
    std::optional<std::vector<uint8_t>> factory = embedded_template(head, tail);
    if (!factory && ns::is_supported_amiibo_dump_size(fallback_template.size())) {
        factory = std::vector<uint8_t>(fallback_template.begin(), fallback_template.end());
    }
    return factory;
}

OperationResult stage_v3_read_prefix(std::span<const uint8_t> tag) {
    g_pending_v3_read_prefix.reset();
    if (tag.size() != V3_SIZE) {
        return {ns::AMIIBO_LIBRARY_OK, static_cast<uint16_t>(tag.size()), {}};
    }
    ns::s2nfc::Signature prefix{ns::s2nfc::Signature::Base{}};
    std::copy_n(tag.begin() + ns::s2nfc::V3_SRAM_OFFSET,
                prefix.size(), prefix.begin());
    g_pending_v3_read_prefix = prefix;
    return {ns::AMIIBO_LIBRARY_OK, static_cast<uint16_t>(tag.size()), {}};
}

} // namespace

OperationResult generate_template(uint32_t head, uint32_t tail,
                                  std::span<const uint8_t> retail_key,
                                  std::vector<uint8_t>& tag) {
    if ((head == 0 && tail == 0) || !validate_key(retail_key)) {
        return {ns::AMIIBO_LIBRARY_INVALID_REQUEST, 0,
                "invalid Amiibo ID or 160-byte build key"};
    }
    const auto generated = generate_tag(
        head, tail,
        std::span<const uint8_t, RETAIL_KEY_SIZE>(
            retail_key.data(), RETAIL_KEY_SIZE));
    if (!generated) {
        return {ns::AMIIBO_LIBRARY_GENERATION_ERROR, 0,
                "OpenSSL could not generate the Amiibo template"};
    }
    tag = *generated;
    return {ns::AMIIBO_LIBRARY_OK, static_cast<uint16_t>(tag.size()), {}};
}

OperationResult select(uint32_t head, uint32_t tail, int console_port,
                       std::vector<uint8_t>& tag,
                       std::span<const uint8_t> fallback_template) {
    std::lock_guard lock(g_mutex);
    g_pending_v3_read_prefix.reset();
    const bool format_requested = (tail & FORMAT_REQUEST_FLAG) != 0;
    tail &= ~FORMAT_REQUEST_FLAG;
    if (console_port < 0 || console_port >= static_cast<int>(g_selected_ids.size())
            || (head == 0 && tail == 0)) {
        return {ns::AMIIBO_LIBRARY_INVALID_REQUEST, 0, "invalid Amiibo selection"};
    }

    const std::string id = tag_id(head, tail);
    auto stored = read_file(tag_path(id));

    if (format_requested) {
        std::string key_error;
        const auto retail_key = load_retail_key(key_error);
        if (!retail_key) {
            return {ns::AMIIBO_LIBRARY_GENERATION_ERROR, 0, std::move(key_error)};
        }

        const auto generated = generate_tag(
            head, tail,
            std::span<const uint8_t, RETAIL_KEY_SIZE>(retail_key->data(),
                                                      retail_key->size()));
        if (!generated) {
            return {ns::AMIIBO_LIBRARY_GENERATION_ERROR, 0,
                    "OpenSSL could not format the selected Amiibo"};
        }
        const OperationResult prefix_result = stage_v3_read_prefix(*generated);
        if (!prefix_result) return prefix_result;
        std::string error;
        if (!atomic_write(tag_path(id), *generated, error)) {
            g_pending_v3_read_prefix.reset();
            return {ns::AMIIBO_LIBRARY_STORAGE_ERROR, 0, std::move(error)};
        }
        tag = *generated;
        g_selected_ids[console_port] = id;
        return {ns::AMIIBO_LIBRARY_OK, static_cast<uint16_t>(tag.size()), {}};
    }

    if (stored && ns::is_supported_amiibo_dump_size(stored->size())) {
        tag = *stored;
        const OperationResult prefix_result = stage_v3_read_prefix(tag);
        if (!prefix_result) return prefix_result;
        g_selected_ids[console_port] = id;
        return {ns::AMIIBO_LIBRARY_OK, static_cast<uint16_t>(tag.size()), {}};
    }

    const auto factory = factory_template(head, tail, fallback_template);
    if (!factory) {
        return {ns::AMIIBO_LIBRARY_GENERATION_ERROR, 0,
                "this build has no factory template for the selected Amiibo"};
    }
    const OperationResult prefix_result = stage_v3_read_prefix(*factory);
    if (!prefix_result) return prefix_result;
    std::string error;
    if (!atomic_write(tag_path(id), *factory, error)) {
        g_pending_v3_read_prefix.reset();
        return {ns::AMIIBO_LIBRARY_STORAGE_ERROR, 0, std::move(error)};
    }
    tag = *factory;
    g_selected_ids[console_port] = id;
    return {ns::AMIIBO_LIBRARY_OK, static_cast<uint16_t>(tag.size()), {}};
}

OperationResult clear() {
    std::lock_guard lock(g_mutex);
    const auto root = library_root();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    if (ec) {
        return {ns::AMIIBO_LIBRARY_STORAGE_ERROR, 0,
                "cannot clear " + root.string() + ": " + ec.message()};
    }
    g_selected_ids.fill({});
    g_pending_v3_read_prefix.reset();
    return {ns::AMIIBO_LIBRARY_OK, 0, {}};
}

bool store_writeback(int console_port, const uint8_t* data, std::size_t len,
                     std::string* error) {
    std::lock_guard lock(g_mutex);
    if (console_port < 0 || console_port >= static_cast<int>(g_selected_ids.size())
            || !data || !ns::is_supported_amiibo_dump_size(len)
            || g_selected_ids[console_port].empty()) {
        if (error) *error = "no persistent Amiibo is selected for this controller";
        return false;
    }
    std::string local_error;
    const bool ok = atomic_write(
        tag_path(g_selected_ids[console_port]),
        std::span<const uint8_t>(data, len), local_error);
    if (error) *error = std::move(local_error);
    return ok;
}

} // namespace amiibo_library

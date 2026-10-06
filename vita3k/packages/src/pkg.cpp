// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

/**
 * @file pkg.cpp
 * @brief PlayStation Vita software package (`.pkg`) handling
 */

#include <F00DKeyEncryptorFactory.h>
#include <PsvPfsParserConfig.h>
#include <Utils.h>
#include <openssl/evp.h>
#include <rif2zrif.h>

#include <io/functions.h>

#include <config/state.h>
#include <emuenv/state.h>

#include <packages/functions.h>
#include <packages/license.h>
#include <packages/pkg.h>

#include <platform/platform.h>

#include <chrono>
#ifdef __PROSPERO__
#include <fcntl.h>
#include <unistd.h>
#endif
#include <packages/sce_types.h>
#include <packages/sfo.h>

#include <util/bytes.h>
#include <util/log.h>

// Credits to mmozeiko https://github.com/mmozeiko/pkg2zip

static void ctr_init(uint8_t *counter, uint8_t *iv, uint64_t n) {
    for (int i = 15; i >= 0; i--) {
        n = n + iv[i];
        counter[i] = (uint8_t)n;
        n >>= 8;
    }
}

static int execute(std::string &zrif, fs::path &title_src, fs::path &title_dst, F00DEncryptorTypes type, std::string &f00d_arg, PfsProgressCallback progress = nullptr) {
    std::string title_src_str = title_src.string();
    std::string title_dst_str = title_dst.string();
    return execute(zrif, title_src_str, title_dst_str, type, f00d_arg, progress);
}

bool decrypt_install_nonpdrm(EmuEnvState &emuenv, const fs::path &drmlicpath, const fs::path &title_path, const std::function<void(float)> &progress_callback) {
    fs::path title_id_src = title_path;
    fs::path title_id_dst = fs_utils::path_concat(title_path, "_dec");
    fs::ifstream binfile(drmlicpath, std::ios::in | std::ios::binary | std::ios::ate);
    std::string zRIF = rif2zrif(binfile);
    F00DEncryptorTypes f00d_enc_type = F00DEncryptorTypes::native;
    std::string f00d_arg = std::string();

    PfsProgressCallback pfs_progress = nullptr;
    if (progress_callback) {
        pfs_progress = [&progress_callback](std::uint64_t processed, std::uint64_t total, const std::string &) {
            progress_callback(total ? static_cast<float>(processed) / static_cast<float>(total) : 1.f);
        };
    }

    if ((execute(zRIF, title_id_src, title_id_dst, f00d_enc_type, f00d_arg, pfs_progress) < 0) && (title_path.string().find("theme") == std::string::npos))
        return false;

    if (!emuenv.app_info.app_category.contains("gp"))
        copy_license(emuenv, drmlicpath);

    fs::remove_all(title_id_src);
    fs::rename(title_id_dst, title_id_src);

    return true;
}

// Writes one file out of a package. A game's data files run to hundreds of megabytes, and writing one in pieces
// makes the filesystem extend it again on every piece. Where the final size is known up front the file is claimed
// in one go first, so the writes that follow only fill space that already belongs to it
class PackageFileWriter {
public:
    PackageFileWriter(const fs::path &path, std::uint64_t total) {
#ifdef __PROSPERO__
        m_fd = ::open(fs_utils::path_to_utf8(path).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (m_fd >= 0 && total > 0 && ::ftruncate(m_fd, static_cast<off_t>(total)) != 0)
            LOG_WARN("Could not claim {} bytes for {} up front", total, path);
#else
        (void)total;
        m_stream.open(path, std::ios::binary);
#endif
    }

    ~PackageFileWriter() {
        close();
    }

    PackageFileWriter(const PackageFileWriter &) = delete;
    PackageFileWriter &operator=(const PackageFileWriter &) = delete;

    bool write(const void *data, std::size_t size) {
#ifdef __PROSPERO__
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        while (size > 0) {
            const ssize_t written = ::write(m_fd, bytes, size);
            if (written <= 0)
                return false;
            bytes += written;
            size -= static_cast<std::size_t>(written);
            m_unflushed += static_cast<std::uint64_t>(written);
        }
        // A game's worth of writing left unflushed slows to a crawl partway through, as what is waiting to be
        // written outgrows the room the system keeps for it. Handing it over in runs keeps that bounded
        if (m_unflushed >= FLUSH_EVERY) {
            ::fsync(m_fd);
            m_unflushed = 0;
        }
        return true;
#else
        m_stream.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
        return m_stream.good();
#endif
    }

    void close() {
#ifdef __PROSPERO__
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
#else
        if (m_stream.is_open())
            m_stream.close();
#endif
    }

private:
#ifdef __PROSPERO__
    static constexpr std::uint64_t FLUSH_EVERY = 64u << 20;
    int m_fd = -1;
    std::uint64_t m_unflushed = 0;
#else
    fs::ofstream m_stream;
#endif
};

bool install_pkg(const fs::path &pkg_path, EmuEnvState &emuenv, std::string &p_zRIF, const std::function<void(float)> &progress_callback, const std::function<void(const std::string &, float)> &item_callback, const std::function<bool()> &cancelled) {
    FILE *infile = FOPEN(pkg_path.c_str(), "rb");
    if (!infile) {
        LOG_CRITICAL("Failed to load pkg file in path: {}", fs_utils::path_to_utf8(pkg_path));
        return false;
    }

    fseek(infile, 0, SEEK_END);
    const uint64_t pkg_size = ftell(infile);

    PkgHeader pkg_header;
    PkgExtHeader ext_header;
    fseek(infile, 0, SEEK_SET);
    fread(reinterpret_cast<void *>(&pkg_header), sizeof(PkgHeader), 1, infile);
    fseek(infile, sizeof(PkgHeader), SEEK_SET);
    fread(reinterpret_cast<char *>(&ext_header), sizeof(PkgExtHeader), 1, infile);

    progress_callback(0);

    if (byte_swap(pkg_header.magic) != 0x7F504b47 && byte_swap(ext_header.magic) != 0x7F657874) {
        LOG_ERROR("Not a valid pkg file!");
        return false;
    }

    if (pkg_size < byte_swap(pkg_header.total_size)) {
        LOG_ERROR("The pkg file is too small");
        return false;
    }

    if (pkg_size < byte_swap(pkg_header.data_offset) + byte_swap(pkg_header.file_count) * 32) {
        LOG_ERROR("The pkg file is too small");
        return false;
    }

    uint32_t info_offset = byte_swap(pkg_header.info_offset);
    uint32_t content_type = 0;
    uint32_t sfo_offset = 0;
    uint32_t sfo_size = 0;
    uint32_t items_offset = 0;

    for (uint32_t i = 0; i < byte_swap(pkg_header.info_count); i++) {
        uint32_t block[4];
        fseek(infile, info_offset, SEEK_SET);
        fread(block, sizeof(block), 1, infile);

        auto type = byte_swap(block[0]);
        auto size = byte_swap(block[1]);

        switch (type) {
        case 2:
            content_type = byte_swap(block[2]);
            break;
        case 13:
            items_offset = byte_swap(block[2]);
            break;
        case 14:
            sfo_offset = byte_swap(block[2]);
            sfo_size = byte_swap(block[3]);
            break;
        default:
            break;
        }

        info_offset += 2 * sizeof(uint32_t) + size;
    }

    PkgType type;

    switch (content_type) {
    case 0x15:
        type = PkgType::PKG_TYPE_VITA_APP;
        break;
    case 0x16:
        type = PkgType::PKG_TYPE_VITA_DLC;
        break;
    case 0x1F:
        type = PkgType::PKG_TYPE_VITA_THEME;
        break;
    default:
        LOG_ERROR("Unsupported content type: {}", content_type);
        return false;
        break;
    }

    auto key_type = byte_swap(ext_header.data_type2) & 7;

    uint8_t main_key[16];
    const uint8_t *pkg_vita_key = nullptr;
    switch (key_type) {
    case 2:
        pkg_vita_key = pkg_vita_2;
        break;
    case 3:
        pkg_vita_key = pkg_vita_3;
        break;
    case 4:
        pkg_vita_key = pkg_vita_4;
        break;
    default:
        LOG_ERROR("Unknown encryption key");
        return false;
        break;
    }

    EVP_CIPHER_CTX *cipher_ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER *cipher_CTR = EVP_CIPHER_fetch(nullptr, "AES-128-CTR", nullptr);
    EVP_CIPHER *cipher_ECB = EVP_CIPHER_fetch(nullptr, "AES-128-ECB", nullptr);
    int dec_len = 0;

    auto evp_cleanup = [&]() {
        EVP_CIPHER_CTX_free(cipher_ctx);
        EVP_CIPHER_free(cipher_CTR);
        EVP_CIPHER_free(cipher_ECB);
    };

    // get the main key
    EVP_EncryptInit_ex(cipher_ctx, cipher_ECB, nullptr, pkg_vita_key, nullptr);
    EVP_CIPHER_CTX_set_padding(cipher_ctx, 0);
    EVP_EncryptUpdate(cipher_ctx, main_key, &dec_len, pkg_header.pkg_data_iv, 0x10);
    EVP_EncryptFinal_ex(cipher_ctx, main_key + dec_len, &dec_len);

    std::vector<uint8_t> sfo_buffer(sfo_size);
    SfoFile sfo_file;
    fseek(infile, sfo_offset, SEEK_SET);
    fread(sfo_buffer.data(), sfo_buffer.size(), 1, infile);
    sfo::load(sfo_file, sfo_buffer);
    sfo::get_param_info(emuenv.app_info, sfo_buffer, emuenv.cfg.sys_lang);

    if (type == PkgType::PKG_TYPE_VITA_DLC)
        emuenv.app_info.app_content_id = emuenv.app_info.app_content_id.substr(20);

    if (type == PkgType::PKG_TYPE_VITA_APP && strcmp(emuenv.app_info.app_category.c_str(), "gp") == 0) {
        type = PkgType::PKG_TYPE_VITA_PATCH;
    }

    auto path{ emuenv.vita_fs_path / "ux0" };

    switch (type) {
    case PkgType::PKG_TYPE_VITA_APP:
        path /= fs::path("app") / emuenv.app_info.app_title_id;
        if (fs::exists(path))
            fs::remove_all(path);
        emuenv.app_info.app_title += " (App)";
        break;
    case PkgType::PKG_TYPE_VITA_DLC:
        path /= fs::path("addcont") / emuenv.app_info.app_title_id / emuenv.app_info.app_content_id;
        emuenv.app_info.app_title += " (DLC)";
        break;
    case PkgType::PKG_TYPE_VITA_PATCH:
        path /= fs::path("patch") / emuenv.app_info.app_title_id;
        emuenv.app_info.app_title += " (Update)";
        break;
    case PkgType::PKG_TYPE_VITA_THEME:
        path /= fs::path("theme") / emuenv.app_info.app_content_id;
        emuenv.app_info.app_category = "theme";
        emuenv.app_info.app_title += " (Theme)";
        break;
    }

    auto decrypt_aes_ctr = [&](uint32_t offset, unsigned char *data, size_t size) {
        uint8_t counter[0x10];
        ctr_init(counter, pkg_header.pkg_data_iv, offset);
        EVP_DecryptInit_ex(cipher_ctx, cipher_CTR, nullptr, main_key, counter);
        EVP_CIPHER_CTX_set_padding(cipher_ctx, 0);
        EVP_DecryptUpdate(cipher_ctx, data, &dec_len, data, size);
        EVP_DecryptFinal_ex(cipher_ctx, data + dec_len, &dec_len);
    };

    // Each pass over this buffer is a read, a decrypt and a write, so a larger one makes fewer round trips
    std::vector<uint8_t> buffer(0x200000);
    for (uint32_t i = 0; i < byte_swap(pkg_header.file_count); i++) {
        PkgEntry entry;
        uint64_t file_offset = items_offset + i * 32;
        fseek(infile, byte_swap(pkg_header.data_offset) + file_offset, SEEK_SET);
        fread(&entry, sizeof(PkgEntry), 1, infile);

        decrypt_aes_ctr(file_offset / 16, reinterpret_cast<unsigned char *>(&entry), sizeof(PkgEntry));

        if (pkg_size < byte_swap(pkg_header.data_offset) + byte_swap(entry.name_offset) + byte_swap(entry.name_size) || pkg_size < byte_swap(pkg_header.data_offset) + byte_swap(entry.data_offset) + byte_swap(entry.data_size)) {
            LOG_ERROR("The pkg file size is too small, possibly corrupted");
            evp_cleanup();
            return false;
        }
        const auto file_count = (float)byte_swap(pkg_header.file_count);
        progress_callback(i / file_count * 100.f * 0.6f);
        std::vector<unsigned char> name(byte_swap(entry.name_size));
        fseek(infile, byte_swap(pkg_header.data_offset) + byte_swap(entry.name_offset), SEEK_SET);
        fread(name.data(), byte_swap(entry.name_size), 1, infile);

        decrypt_aes_ctr(byte_swap(entry.name_offset) / 16, name.data(), byte_swap(entry.name_size));

        auto string_name = std::string(name.begin(), name.end());
        LOG_INFO(string_name);
        if (item_callback)
            item_callback(string_name, 0.f);

        if ((byte_swap(entry.type) & 0xFF) == 4 || (byte_swap(entry.type) & 0xFF) == 18) { // Directory
            fs::create_directories(path / string_name);
        } else { // File
            auto offset = byte_swap(entry.data_offset);
            auto data_size = byte_swap(entry.data_size);
            PackageFileWriter outfile(path / string_name, data_size);

            uint8_t counter[0x10];
            ctr_init(counter, pkg_header.pkg_data_iv, offset / 16);
            EVP_DecryptInit_ex(cipher_ctx, cipher_CTR, nullptr, main_key, counter);
            EVP_CIPHER_CTX_set_padding(cipher_ctx, 0);

            fseek(infile, byte_swap(pkg_header.data_offset) + offset, SEEK_SET);
            // A game's largest files run to hundreds of megabytes, so the share of this one that is done counts
            // towards the whole: without it the report stands still for as long as one file takes
            const float file_share = 100.f * 0.6f / file_count;
            const uint64_t file_bytes = data_size;
            // Where a large file's time actually goes, so a slow install can be blamed on the right step
            std::chrono::nanoseconds reading{ 0 }, decrypting{ 0 }, writing{ 0 };
            while (data_size != 0) {
                size_t size = data_size < buffer.size() ? data_size : buffer.size();
                auto mark = std::chrono::steady_clock::now();
                fread(buffer.data(), size, 1, infile);
                const auto read_done = std::chrono::steady_clock::now();
                reading += read_done - mark;

                EVP_DecryptUpdate(cipher_ctx, buffer.data(), &dec_len, buffer.data(), size);
                const auto decrypt_done = std::chrono::steady_clock::now();
                decrypting += decrypt_done - read_done;

                outfile.write(buffer.data(), dec_len);
                writing += std::chrono::steady_clock::now() - decrypt_done;
                data_size -= size;
                const float of_this_file = static_cast<float>(file_bytes - data_size) / static_cast<float>(file_bytes);
                progress_callback(i / file_count * 100.f * 0.6f + file_share * of_this_file);
                if (item_callback)
                    item_callback(string_name, of_this_file);
                if (cancelled && cancelled()) {
                    // Leave nothing half written behind: what was unpacked so far is of no use on its own
                    outfile.close();
                    fclose(infile);
                    evp_cleanup();
                    LOG_INFO("Installation of {} cancelled", pkg_path);
                    // The console has refused a removal before now, and throwing here would take the emulator with it
                    if (const int left = platform::remove_tree(fs_utils::path_to_utf8(path)); left > 0)
                        LOG_WARN("{} entries of {} would not go", left, path);
                    return false;
                }
            }

            if (file_bytes >= (32u << 20)) {
                const auto ms = [](std::chrono::nanoseconds d) { return std::chrono::duration<double, std::milli>(d).count(); };
                LOG_INFO("{} ({} MB): read {:.0f} ms, decrypt {:.0f} ms, write {:.0f} ms", string_name,
                    file_bytes >> 20, ms(reading), ms(decrypting), ms(writing));
            }

            EVP_DecryptFinal_ex(cipher_ctx, buffer.data(), &dec_len);
            outfile.write(buffer.data(), dec_len);
            outfile.close();
        }
    }
    fclose(infile);

    evp_cleanup();
    fs::path title_id_src = path;
    fs::path title_id_dst = fs_utils::path_concat(path, "_dec");
    std::string zRIF = p_zRIF;
    F00DEncryptorTypes f00d_enc_type = F00DEncryptorTypes::native;
    std::string f00d_arg = std::string();

    progress_callback(80);
    // Decrypting is the rest of the work and used to report nothing, so the bar sat at 80 until it was done
    // The decryption reports once per file, with the bytes done so far out of the whole. There is nothing finer to
    // show inside one file, so the row for the file being worked on carries how far through the whole it is, which
    // at least moves while a large one is being written
    const auto decrypt_started = std::chrono::steady_clock::now();
    auto last_report = decrypt_started;
    std::uint64_t last_processed = 0;
    const auto pfs_progress = [&](std::uint64_t processed, std::uint64_t total, const std::string &file) {
        const float done = total > 0 ? static_cast<float>(processed) / static_cast<float>(total) : 0.f;
        progress_callback(80.f + 20.f * done);
        if (item_callback && !file.empty())
            item_callback(file, done);

        // How fast the decryption is actually going, once every few seconds
        const auto now = std::chrono::steady_clock::now();
        const double since = std::chrono::duration<double>(now - last_report).count();
        if (since >= 5.0) {
            LOG_INFO("Decrypting at {:.1f} MB/s ({} of {} MB)", (processed - last_processed) / 1e6 / since,
                processed >> 20, total >> 20);
            last_report = now;
            last_processed = processed;
        }
    };

    switch (type) {
    case PkgType::PKG_TYPE_VITA_APP:
    case PkgType::PKG_TYPE_VITA_PATCH:

        if (execute(zRIF, title_id_src, title_id_dst, f00d_enc_type, f00d_arg, pfs_progress) < 0) {
            // The package has already been unpacked and its own layer of encryption taken off. This second pass is
            // the one that failed, so what was written stays: throwing away a game's worth of work would be worse
            // than handing over content that may or may not need it. Only the half-made copy goes
            LOG_ERROR("The PFS layer of {} could not be decrypted; keeping what was unpacked", title_id_src);
            platform::remove_tree(fs_utils::path_to_utf8(title_id_dst));
            return true;
        }
        platform::remove_tree(fs_utils::path_to_utf8(title_id_src));
        fs::rename(title_id_dst, title_id_src);

        break;
    case PkgType::PKG_TYPE_VITA_DLC:

        if (execute(zRIF, title_id_src, title_id_dst, f00d_enc_type, f00d_arg, pfs_progress) < 0) {
            platform::remove_tree(fs_utils::path_to_utf8(title_id_src));
            platform::remove_tree(fs_utils::path_to_utf8(title_id_dst));
            return false;
        } else {
            platform::remove_tree(fs_utils::path_to_utf8(title_id_src));
            fs::rename(title_id_dst, title_id_src);
            return true;
        }
        break;

    case PkgType::PKG_TYPE_VITA_THEME:

        // Theme don't have keystone file, need skip error
        execute(zRIF, title_id_src, title_id_dst, f00d_enc_type, f00d_arg, pfs_progress);
        platform::remove_tree(fs_utils::path_to_utf8(title_id_src));
        fs::rename(title_id_dst, title_id_src);
        return true;
        break;
    }

    if (!copy_path(title_id_src, emuenv.vita_fs_path, emuenv.app_info.app_title_id, emuenv.app_info.app_category))
        return false;

    create_license(emuenv, zRIF);

    progress_callback(100);
    return true;
}

// How fast this build can actually decrypt. Installing a package is almost entirely AES, and OpenSSL picks its
// hardware implementation from a CPUID check that runs as the library starts. Where that check never runs it falls
// back to portable C an order of magnitude slower, and this tells the two apart: hardware AES reaches gigabytes a
// second, the fallback tens of megabytes
void report_crypto_speed() {
    constexpr int BYTES = 16 << 20;
    std::vector<unsigned char> data(BYTES, 0x5a);
    unsigned char key[32] = {}, iv[16] = {};

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER *cipher = EVP_CIPHER_fetch(nullptr, "AES-256-CTR", nullptr);
    const auto started = std::chrono::steady_clock::now();
    int length = 0;
    if (ctx && cipher && EVP_EncryptInit_ex(ctx, cipher, nullptr, key, iv) == 1)
        EVP_EncryptUpdate(ctx, data.data(), &length, data.data(), BYTES);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    EVP_CIPHER_free(cipher);
    EVP_CIPHER_CTX_free(ctx);

    LOG_INFO("Crypto: AES-256-CTR at {:.0f} MB/s", seconds > 0 ? BYTES / 1e6 / seconds : 0.0);
}

std::string find_pkg_zrif(const fs::path &pkg_path, const fs::path &vita_fs_path) {
    FILE *infile = FOPEN(pkg_path.c_str(), "rb");
    if (!infile)
        return {};

    PkgHeader pkg_header{};
    fread(&pkg_header, sizeof(PkgHeader), 1, infile);
    fclose(infile);

    const std::string content_id(pkg_header.content_id);
    if (content_id.size() < 16)
        return {};

    const std::string title_id = content_id.substr(7, 9);
    const auto rif_path = vita_fs_path / "ux0/license" / title_id / (content_id + ".rif");

    if (!fs::exists(rif_path))
        return {};

    LOG_INFO("Found license file: {}", rif_path);
    fs::ifstream binfile(rif_path, std::ios::in | std::ios::binary | std::ios::ate);
    if (!binfile)
        return {};

    return rif2zrif(binfile);
}

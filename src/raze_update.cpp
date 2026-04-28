#include <STManager/raze_update.h>
#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

#include "path_safety.h"
#include "platform_compat.h"

namespace STManager {
namespace {

const char* const kDefaultRepository = "arkuna23/SillyTavernRaze";
const char* const kHashAlgorithmSha256 = "sha256";
const size_t kDownloadBufferSize = 64 * 1024;

std::string join_path(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs[lhs.size() - 1] == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

std::string normalize_archive_path(const std::string& path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

std::string to_lower_ascii(const std::string& value) {
    std::string lowered = value;
    for (std::string::size_type index = 0; index < lowered.size(); ++index) {
        if (lowered[index] >= 'A' && lowered[index] <= 'Z') {
            lowered[index] = static_cast<char>(lowered[index] - 'A' + 'a');
        }
    }
    return lowered;
}

Status make_curl_error(const std::string& stage, CURLcode code, const char* error_buffer) {
    std::ostringstream message;
    message << "stage=" << stage << "; curl error " << static_cast<int>(code) << ": ";
    if (error_buffer != NULL && error_buffer[0] != '\0') {
        message << error_buffer;
    } else {
        message << curl_easy_strerror(code);
    }
    return Status(StatusCode::kNetworkError, message.str());
}

Status make_http_error(const std::string& stage, long response_code) {
    std::ostringstream message;
    message << "stage=" << stage << "; HTTP " << response_code;
    return Status(StatusCode::kNetworkError, message.str());
}

size_t write_string_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (userdata == NULL) {
        return 0;
    }
    const size_t byte_count = size * nmemb;
    std::string* output = static_cast<std::string*>(userdata);
    output->append(ptr, byte_count);
    return byte_count;
}

size_t write_stream_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (userdata == NULL) {
        return 0;
    }
    const size_t byte_count = size * nmemb;
    std::ostream* output = static_cast<std::ostream*>(userdata);
    output->write(ptr, static_cast<std::streamsize>(byte_count));
    return output->good() ? byte_count : 0;
}

Status configure_curl_common(CURL* curl, const std::string& url, char* error_buffer) {
    if (curl == NULL) {
        return Status(StatusCode::kNetworkError, "stage=http.init; curl_easy_init failed");
    }

    error_buffer[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "STManager/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    return Status::ok_status();
}

Status http_get_string(const std::string& url, std::string* output) {
    if (output == NULL) {
        return Status(StatusCode::kNetworkError, "stage=http.string.args; output cannot be null");
    }

    output->clear();
    CURL* curl = curl_easy_init();
    char error_buffer[CURL_ERROR_SIZE];
    const Status configure_status = configure_curl_common(curl, url, error_buffer);
    if (!configure_status.ok()) {
        return configure_status;
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, output);

    const CURLcode perform_code = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);

    if (perform_code != CURLE_OK) {
        return make_curl_error("http.string.perform", perform_code, error_buffer);
    }
    if (response_code < 200 || response_code >= 300) {
        return make_http_error("http.string.response", response_code);
    }

    return Status::ok_status();
}

Status http_download_file(const std::string& url, const std::string& destination_file) {
    const Status parent_status = internal::ensure_parent_directories(destination_file, 0755);
    if (!parent_status.ok()) {
        return parent_status;
    }

    std::ofstream output(destination_file.c_str(), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return Status(StatusCode::kIoError,
                      "stage=http.file.open; failed opening " + destination_file);
    }

    CURL* curl = curl_easy_init();
    char error_buffer[CURL_ERROR_SIZE];
    const Status configure_status = configure_curl_common(curl, url, error_buffer);
    if (!configure_status.ok()) {
        return configure_status;
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);

    const CURLcode perform_code = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);

    output.flush();
    if (!output.good()) {
        return Status(StatusCode::kIoError,
                      "stage=http.file.flush; failed writing " + destination_file);
    }

    if (perform_code != CURLE_OK) {
        std::remove(destination_file.c_str());
        return make_curl_error("http.file.perform", perform_code, error_buffer);
    }
    if (response_code < 200 || response_code >= 300) {
        std::remove(destination_file.c_str());
        return make_http_error("http.file.response", response_code);
    }

    return Status::ok_status();
}

std::string github_releases_api_url(const std::string& repository) {
    return "https://api.github.com/repos/" + repository + "/releases?per_page=10";
}

Status extract_regular_file(struct archive* archive_reader, const std::string& destination_path,
                            mode_t mode) {
    const Status parent_status = internal::ensure_parent_directories(destination_path, 0755);
    if (!parent_status.ok()) {
        return parent_status;
    }

    std::ofstream output(destination_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return Status(StatusCode::kIoError,
                      "stage=raze.extract.file.open; failed opening " + destination_path);
    }

    std::vector<char> buffer(kDownloadBufferSize);
    while (true) {
        const la_ssize_t read_count =
            archive_read_data(archive_reader, buffer.data(), buffer.size());
        if (read_count == 0) {
            break;
        }
        if (read_count < 0) {
            return Status(StatusCode::kArchiveError, std::string("stage=raze.extract.file.read; ") +
                                                         archive_error_string(archive_reader));
        }
        output.write(buffer.data(), static_cast<std::streamsize>(read_count));
        if (!output.good()) {
            return Status(StatusCode::kIoError,
                          "stage=raze.extract.file.write; failed writing " + destination_path);
        }
    }

    output.close();
#ifndef _WIN32
    chmod(destination_path.c_str(), mode & 0777);
#endif
    return Status::ok_status();
}

Status validate_release_asset(const nlohmann::json& asset, RazeReleaseInfo* release_info) {
    if (!asset.is_object()) {
        return Status::ok_status();
    }
    if (!asset.contains("name") || !asset["name"].is_string()) {
        return Status::ok_status();
    }
    if (!asset.contains("browser_download_url") || !asset["browser_download_url"].is_string()) {
        return Status::ok_status();
    }

    const std::string name = asset["name"].get<std::string>();
    const std::string download_url = asset["browser_download_url"].get<std::string>();
    release_info->asset_download_urls[name] = download_url;
    if (name == "version.json") {
        release_info->version_json_url = download_url;
    }
    return Status::ok_status();
}

Status parse_release_json(const std::string& release_json, RazeReleaseInfo* release_info) {
    if (release_info == NULL) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.release.args; release_info cannot be null");
    }

    release_info->tag_name.clear();
    release_info->version_json_url.clear();
    release_info->asset_download_urls.clear();

    nlohmann::json releases;
    try {
        releases = nlohmann::json::parse(release_json);
    } catch (const std::exception& exception) {
        return Status(StatusCode::kInvalidManifest,
                      std::string("stage=raze.release.parse; ") + exception.what());
    }

    if (!releases.is_array()) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.release.type; releases response is not an array");
    }

    for (nlohmann::json::const_iterator it = releases.begin(); it != releases.end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        if (it->contains("draft") && (*it)["draft"].is_boolean() && (*it)["draft"].get<bool>()) {
            continue;
        }
        if (!it->contains("tag_name") || !(*it)["tag_name"].is_string()) {
            continue;
        }
        if (!it->contains("assets") || !(*it)["assets"].is_array()) {
            continue;
        }

        release_info->tag_name = (*it)["tag_name"].get<std::string>();
        for (nlohmann::json::const_iterator asset_it = (*it)["assets"].begin();
             asset_it != (*it)["assets"].end(); ++asset_it) {
            validate_release_asset(*asset_it, release_info);
        }

        if (release_info->version_json_url.empty()) {
            return Status(StatusCode::kInvalidManifest,
                          "stage=raze.release.assets; latest release has no version.json asset");
        }
        return Status::ok_status();
    }

    return Status(StatusCode::kInvalidManifest,
                  "stage=raze.release.empty; no usable release found");
}

#define SHA256_ROTLEFT(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define SHA256_ROTRIGHT(a, b) (((a) >> (b)) | ((a) << (32 - (b))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x) (SHA256_ROTRIGHT(x, 2) ^ SHA256_ROTRIGHT(x, 13) ^ SHA256_ROTRIGHT(x, 22))
#define SHA256_EP1(x) (SHA256_ROTRIGHT(x, 6) ^ SHA256_ROTRIGHT(x, 11) ^ SHA256_ROTRIGHT(x, 25))
#define SHA256_SIG0(x) (SHA256_ROTRIGHT(x, 7) ^ SHA256_ROTRIGHT(x, 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (SHA256_ROTRIGHT(x, 17) ^ SHA256_ROTRIGHT(x, 19) ^ ((x) >> 10))

class Sha256Context {
public:
    Sha256Context() : data_(), data_len_(0), bit_len_(0), state_() {
        state_[0] = 0x6a09e667;
        state_[1] = 0xbb67ae85;
        state_[2] = 0x3c6ef372;
        state_[3] = 0xa54ff53a;
        state_[4] = 0x510e527f;
        state_[5] = 0x9b05688c;
        state_[6] = 0x1f83d9ab;
        state_[7] = 0x5be0cd19;
    }

    void update(const unsigned char* data, size_t length) {
        for (size_t index = 0; index < length; ++index) {
            data_[data_len_] = data[index];
            ++data_len_;
            if (data_len_ == 64) {
                transform();
                bit_len_ += 512;
                data_len_ = 0;
            }
        }
    }

    void final(unsigned char* hash) {
        size_t index = data_len_;

        if (data_len_ < 56) {
            data_[index++] = 0x80;
            while (index < 56) {
                data_[index++] = 0x00;
            }
        } else {
            data_[index++] = 0x80;
            while (index < 64) {
                data_[index++] = 0x00;
            }
            transform();
            std::memset(data_, 0, 56);
        }

        bit_len_ += static_cast<uint64_t>(data_len_) * 8;
        data_[63] = static_cast<unsigned char>(bit_len_);
        data_[62] = static_cast<unsigned char>(bit_len_ >> 8);
        data_[61] = static_cast<unsigned char>(bit_len_ >> 16);
        data_[60] = static_cast<unsigned char>(bit_len_ >> 24);
        data_[59] = static_cast<unsigned char>(bit_len_ >> 32);
        data_[58] = static_cast<unsigned char>(bit_len_ >> 40);
        data_[57] = static_cast<unsigned char>(bit_len_ >> 48);
        data_[56] = static_cast<unsigned char>(bit_len_ >> 56);
        transform();

        for (index = 0; index < 4; ++index) {
            for (size_t state_index = 0; state_index < 8; ++state_index) {
                hash[index + (state_index * 4)] =
                    static_cast<unsigned char>((state_[state_index] >> (24 - index * 8)) & 0xff);
            }
        }
    }

private:
    void transform() {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};

        uint32_t message[64];
        for (size_t index = 0, word_index = 0; word_index < 16; ++word_index, index += 4) {
            message[word_index] = (static_cast<uint32_t>(data_[index]) << 24) |
                                  (static_cast<uint32_t>(data_[index + 1]) << 16) |
                                  (static_cast<uint32_t>(data_[index + 2]) << 8) |
                                  (static_cast<uint32_t>(data_[index + 3]));
        }
        for (size_t index = 16; index < 64; ++index) {
            message[index] = SHA256_SIG1(message[index - 2]) + message[index - 7] +
                             SHA256_SIG0(message[index - 15]) + message[index - 16];
        }

        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];

        for (size_t index = 0; index < 64; ++index) {
            const uint32_t t1 = h + SHA256_EP1(e) + SHA256_CH(e, f, g) + k[index] + message[index];
            const uint32_t t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    unsigned char data_[64];
    size_t data_len_;
    uint64_t bit_len_;
    uint32_t state_[8];
};

#undef SHA256_ROTLEFT
#undef SHA256_ROTRIGHT
#undef SHA256_CH
#undef SHA256_MAJ
#undef SHA256_EP0
#undef SHA256_EP1
#undef SHA256_SIG0
#undef SHA256_SIG1

}  // namespace

std::string default_raze_repository() {
    return kDefaultRepository;
}

RazeUpdateOptions::RazeUpdateOptions() : root_path(), repository(kDefaultRepository), cache_dir() {}

RazePackageUpdate::RazePackageUpdate()
    : name(),
      file(),
      download_url(),
      local_hash(),
      remote_hash(),
      reason(),
      needs_download(false) {}

RazeUpdateResult::RazeUpdateResult()
    : local_version(),
      remote_version(),
      initialized(false),
      downloaded_count(0),
      skipped_count(0),
      package_updates() {}

Status fetch_latest_raze_release(const RazeUpdateOptions& options, RazeReleaseInfo* release_info) {
    const std::string repository =
        options.repository.empty() ? kDefaultRepository : options.repository;
    std::string release_json;
    const Status download_status =
        http_get_string(github_releases_api_url(repository), &release_json);
    if (!download_status.ok()) {
        return download_status;
    }
    return parse_release_json(release_json, release_info);
}

Status download_raze_version_manifest(const RazeReleaseInfo& release_info,
                                      RazeVersionManifest* manifest) {
    if (release_info.version_json_url.empty()) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.manifest.url; version_json_url is empty");
    }

    std::string manifest_json;
    const Status download_status = http_get_string(release_info.version_json_url, &manifest_json);
    if (!download_status.ok()) {
        return download_status;
    }
    return parse_raze_version_manifest(manifest_json, manifest);
}

Status parse_raze_version_manifest(const std::string& manifest_json,
                                   RazeVersionManifest* manifest) {
    if (manifest == NULL) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.manifest.args; manifest cannot be null");
    }

    RazeVersionManifest parsed_manifest;
    parsed_manifest.raw_json = manifest_json;

    nlohmann::json json_manifest;
    try {
        json_manifest = nlohmann::json::parse(manifest_json);
    } catch (const std::exception& exception) {
        return Status(StatusCode::kInvalidManifest,
                      std::string("stage=raze.manifest.parse; ") + exception.what());
    }

    if (!json_manifest.is_object()) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.manifest.type; manifest is not an object");
    }
    if (!json_manifest.contains("version") || !json_manifest["version"].is_string()) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.manifest.version; version is missing");
    }
    if (!json_manifest.contains("hashAlgorithm") || !json_manifest["hashAlgorithm"].is_string()) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.manifest.hash_algorithm; hashAlgorithm is missing");
    }
    if (!json_manifest.contains("packages") || !json_manifest["packages"].is_object()) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.manifest.packages; packages is missing");
    }

    parsed_manifest.version = json_manifest["version"].get<std::string>();
    parsed_manifest.hash_algorithm = json_manifest["hashAlgorithm"].get<std::string>();
    if (to_lower_ascii(parsed_manifest.hash_algorithm) != kHashAlgorithmSha256) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.manifest.hash_algorithm; only sha256 is supported");
    }

    const nlohmann::json& packages = json_manifest["packages"];
    for (nlohmann::json::const_iterator it = packages.begin(); it != packages.end(); ++it) {
        if (!it.value().is_object()) {
            return Status(StatusCode::kInvalidManifest,
                          "stage=raze.manifest.package; package entry is not an object");
        }
        if (!it.value().contains("file") || !it.value()["file"].is_string()) {
            return Status(StatusCode::kInvalidManifest,
                          "stage=raze.manifest.package.file; package file is missing");
        }
        if (!it.value().contains("hash") || !it.value()["hash"].is_string()) {
            return Status(StatusCode::kInvalidManifest,
                          "stage=raze.manifest.package.hash; package hash is missing");
        }

        RazePackageInfo package_info;
        package_info.name = it.key();
        package_info.file = it.value()["file"].get<std::string>();
        package_info.hash = to_lower_ascii(it.value()["hash"].get<std::string>());
        parsed_manifest.packages[package_info.name] = package_info;
    }

    if (parsed_manifest.packages.empty()) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.manifest.packages.empty; packages cannot be empty");
    }

    *manifest = parsed_manifest;
    return Status::ok_status();
}

Status read_local_raze_version_manifest(const std::string& root_path, bool* exists,
                                        RazeVersionManifest* manifest) {
    if (exists == NULL || manifest == NULL) {
        return Status(StatusCode::kInvalidManifest, "stage=raze.local.args; output cannot be null");
    }

    *exists = false;
    *manifest = RazeVersionManifest();

    const std::string manifest_path = join_path(root_path, "version.json");
    std::ifstream input(manifest_path.c_str(), std::ios::binary);
    if (!input.is_open()) {
        struct stat manifest_stat;
        if (stat(manifest_path.c_str(), &manifest_stat) != 0 && errno == ENOENT) {
            return Status::ok_status();
        }
        return Status(StatusCode::kIoError,
                      "stage=raze.local.open; failed opening " + manifest_path);
    }

    std::string manifest_json;
    char buffer[4096];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
        manifest_json.append(buffer, static_cast<std::string::size_type>(input.gcount()));
    }
    if (!input.eof()) {
        return Status(StatusCode::kIoError,
                      "stage=raze.local.read; failed reading " + manifest_path);
    }

    const Status parse_status = parse_raze_version_manifest(manifest_json, manifest);
    if (!parse_status.ok()) {
        return parse_status;
    }

    *exists = true;
    return Status::ok_status();
}

Status write_local_raze_version_manifest(const std::string& root_path,
                                         const RazeVersionManifest& manifest) {
    const Status root_status = internal::ensure_directory_tree(root_path, 0755);
    if (!root_status.ok()) {
        return root_status;
    }

    const std::string manifest_path = join_path(root_path, "version.json");
    std::ofstream output(manifest_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return Status(StatusCode::kIoError,
                      "stage=raze.local.write.open; failed opening " + manifest_path);
    }

    output << manifest.raw_json;
    output.flush();
    if (!output.good()) {
        return Status(StatusCode::kIoError,
                      "stage=raze.local.write.flush; failed writing " + manifest_path);
    }

    return Status::ok_status();
}

Status plan_raze_package_updates(const RazeVersionManifest& remote_manifest,
                                 bool has_local_manifest, const RazeVersionManifest& local_manifest,
                                 const RazeReleaseInfo& release_info,
                                 std::vector<RazePackageUpdate>* package_updates) {
    if (package_updates == NULL) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.plan.args; package_updates cannot be null");
    }

    package_updates->clear();
    for (std::map<std::string, RazePackageInfo>::const_iterator it =
             remote_manifest.packages.begin();
         it != remote_manifest.packages.end(); ++it) {
        RazePackageUpdate update;
        update.name = it->second.name;
        update.file = it->second.file;
        update.remote_hash = it->second.hash;

        const std::map<std::string, RazePackageInfo>::const_iterator local_it =
            local_manifest.packages.find(update.name);
        if (!has_local_manifest) {
            update.needs_download = true;
            update.reason = "initial install";
        } else if (local_it == local_manifest.packages.end()) {
            update.needs_download = true;
            update.reason = "missing locally";
        } else {
            update.local_hash = local_it->second.hash;
            update.needs_download =
                to_lower_ascii(local_it->second.hash) != to_lower_ascii(update.remote_hash);
            update.reason = update.needs_download ? "hash changed" : "unchanged";
        }

        const std::map<std::string, std::string>::const_iterator asset_it =
            release_info.asset_download_urls.find(update.file);
        if (update.needs_download) {
            if (asset_it == release_info.asset_download_urls.end()) {
                return Status(StatusCode::kInvalidManifest,
                              "stage=raze.plan.asset; missing release asset for " + update.file);
            }
            update.download_url = asset_it->second;
        } else if (asset_it != release_info.asset_download_urls.end()) {
            update.download_url = asset_it->second;
        }

        package_updates->push_back(update);
    }

    return Status::ok_status();
}

Status download_raze_package(const std::string& download_url, const std::string& destination_file) {
    if (download_url.empty() || destination_file.empty()) {
        return Status(StatusCode::kNetworkError,
                      "stage=raze.package.download.args; url and destination_file are required");
    }
    return http_download_file(download_url, destination_file);
}

Status compute_file_sha256(const std::string& file_path, std::string* hash_hex) {
    if (hash_hex == NULL) {
        return Status(StatusCode::kHashMismatch, "stage=raze.sha256.args; hash_hex cannot be null");
    }

    std::ifstream input(file_path.c_str(), std::ios::binary);
    if (!input.is_open()) {
        return Status(StatusCode::kIoError, "stage=raze.sha256.open; failed opening " + file_path);
    }

    Sha256Context context;
    std::vector<unsigned char> buffer(kDownloadBufferSize);
    while (input.read(reinterpret_cast<char*>(buffer.data()),
                      static_cast<std::streamsize>(buffer.size())) ||
           input.gcount() > 0) {
        context.update(buffer.data(), static_cast<size_t>(input.gcount()));
    }
    if (!input.eof()) {
        return Status(StatusCode::kIoError, "stage=raze.sha256.read; failed reading " + file_path);
    }

    unsigned char hash[32];
    context.final(hash);

    std::ostringstream hash_stream;
    hash_stream << std::hex << std::setfill('0');
    for (size_t index = 0; index < sizeof(hash); ++index) {
        hash_stream << std::setw(2) << static_cast<unsigned int>(hash[index]);
    }
    *hash_hex = hash_stream.str();
    return Status::ok_status();
}

Status verify_raze_package_file(const std::string& file_path, const std::string& hash_algorithm,
                                const std::string& expected_hash) {
    if (to_lower_ascii(hash_algorithm) != kHashAlgorithmSha256) {
        return Status(StatusCode::kInvalidManifest,
                      "stage=raze.verify.algorithm; only sha256 is supported");
    }

    std::string actual_hash;
    const Status hash_status = compute_file_sha256(file_path, &actual_hash);
    if (!hash_status.ok()) {
        return hash_status;
    }

    if (to_lower_ascii(actual_hash) != to_lower_ascii(expected_hash)) {
        std::ostringstream message;
        message << "stage=raze.verify.hash; hash mismatch for " << file_path
                << "; expected=" << expected_hash << "; actual=" << actual_hash;
        return Status(StatusCode::kHashMismatch, message.str());
    }

    return Status::ok_status();
}

Status extract_raze_package(const std::string& archive_file, const std::string& root_path) {
    const Status root_status = internal::ensure_directory_tree(root_path, 0755);
    if (!root_status.ok()) {
        return root_status;
    }

    struct archive* archive_reader = archive_read_new();
    if (archive_reader == NULL) {
        return Status(StatusCode::kArchiveError,
                      "stage=raze.extract.init; archive_read_new failed");
    }

    archive_read_support_filter_none(archive_reader);
    archive_read_support_filter_gzip(archive_reader);
    archive_read_support_filter_zstd(archive_reader);
    archive_read_support_format_tar(archive_reader);
    archive_read_support_format_zip(archive_reader);

    if (archive_read_open_filename(archive_reader, archive_file.c_str(), kDownloadBufferSize) !=
        ARCHIVE_OK) {
        const std::string archive_error = archive_error_string(archive_reader) == NULL
                                              ? "unknown archive open error"
                                              : archive_error_string(archive_reader);
        archive_read_free(archive_reader);
        return Status(StatusCode::kArchiveError, "stage=raze.extract.open; " + archive_error);
    }

    struct archive_entry* entry = NULL;
    while (true) {
        const int next_result = archive_read_next_header(archive_reader, &entry);
        if (next_result == ARCHIVE_EOF) {
            break;
        }
        if (next_result != ARCHIVE_OK) {
            const std::string archive_error = archive_error_string(archive_reader) == NULL
                                                  ? "unknown archive read error"
                                                  : archive_error_string(archive_reader);
            archive_read_close(archive_reader);
            archive_read_free(archive_reader);
            return Status(StatusCode::kArchiveError, "stage=raze.extract.header; " + archive_error);
        }

        const char* raw_path = archive_entry_pathname_utf8(entry);
        if (raw_path == NULL) {
            raw_path = archive_entry_pathname(entry);
        }
        const std::string archive_path =
            normalize_archive_path(raw_path == NULL ? std::string() : raw_path);
        std::string destination_path;
        const Status path_status =
            internal::join_destination_path(root_path, archive_path, &destination_path);
        if (!path_status.ok()) {
            archive_read_close(archive_reader);
            archive_read_free(archive_reader);
            return path_status;
        }

        const mode_t entry_mode = archive_entry_mode(entry);
        if (S_ISDIR(entry_mode)) {
            const Status directory_status =
                internal::ensure_directory_tree(destination_path, entry_mode & 0777);
            if (!directory_status.ok()) {
                archive_read_close(archive_reader);
                archive_read_free(archive_reader);
                return directory_status;
            }
        } else if (S_ISREG(entry_mode)) {
            const Status file_status =
                extract_regular_file(archive_reader, destination_path, entry_mode);
            if (!file_status.ok()) {
                archive_read_close(archive_reader);
                archive_read_free(archive_reader);
                return file_status;
            }
        } else {
            archive_read_close(archive_reader);
            archive_read_free(archive_reader);
            return Status(StatusCode::kUnsupportedArchiveEntry,
                          "stage=raze.extract.entry; unsupported archive entry: " + archive_path);
        }
    }

    archive_read_close(archive_reader);
    archive_read_free(archive_reader);
    return Status::ok_status();
}

}  // namespace STManager

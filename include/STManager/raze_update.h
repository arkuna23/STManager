#ifndef STMANAGER_RAZE_UPDATE_HPP
#define STMANAGER_RAZE_UPDATE_HPP

#include <STManager/data.h>
#include <STManager/stmanager_export.h>

#include <map>
#include <string>
#include <vector>

namespace STManager {

/** Default GitHub repository used for SillyTavernRaze updates. */
STMANAGER_EXPORT std::string default_raze_repository();

/** Options shared by SillyTavernRaze update helper functions. */
struct RazeUpdateOptions {
    std::string root_path;
    std::string repository;
    std::string cache_dir;

    RazeUpdateOptions();
};

/** GitHub release metadata needed to download manifest and package assets. */
struct RazeReleaseInfo {
    std::string tag_name;
    std::string version_json_url;
    std::map<std::string, std::string> asset_download_urls;
};

/** One package entry from version.json. */
struct RazePackageInfo {
    std::string name;
    std::string file;
    std::string hash;
};

/** Parsed SillyTavernRaze version.json manifest. */
struct RazeVersionManifest {
    std::string version;
    std::string hash_algorithm;
    std::map<std::string, RazePackageInfo> packages;
    std::string raw_json;
};

/** A package action derived by comparing local and remote manifests. */
struct RazePackageUpdate {
    std::string name;
    std::string file;
    std::string download_url;
    std::string local_hash;
    std::string remote_hash;
    std::string reason;
    bool needs_download;

    RazePackageUpdate();
};

/** Summary emitted by CLI orchestration after applying update helpers. */
struct RazeUpdateResult {
    std::string local_version;
    std::string remote_version;
    bool initialized;
    size_t downloaded_count;
    size_t skipped_count;
    std::vector<RazePackageUpdate> package_updates;

    RazeUpdateResult();
};

/** Fetch the newest non-draft GitHub release, including prereleases. */
STMANAGER_EXPORT Status fetch_latest_raze_release(const RazeUpdateOptions& options,
                                                  RazeReleaseInfo* release_info);

/** Download and parse the release version.json asset. */
STMANAGER_EXPORT Status download_raze_version_manifest(const RazeReleaseInfo& release_info,
                                                       RazeVersionManifest* manifest);

/** Parse version.json text into a manifest structure. */
STMANAGER_EXPORT Status parse_raze_version_manifest(const std::string& manifest_json,
                                                    RazeVersionManifest* manifest);

/** Read <root>/version.json. exists is false when the file is absent. */
STMANAGER_EXPORT Status read_local_raze_version_manifest(const std::string& root_path, bool* exists,
                                                         RazeVersionManifest* manifest);

/** Write manifest.raw_json to <root>/version.json after successful package extraction. */
STMANAGER_EXPORT Status write_local_raze_version_manifest(const std::string& root_path,
                                                          const RazeVersionManifest& manifest);

/** Compare manifests and resolve package asset URLs from the selected release. */
STMANAGER_EXPORT Status plan_raze_package_updates(const RazeVersionManifest& remote_manifest,
                                                  bool has_local_manifest,
                                                  const RazeVersionManifest& local_manifest,
                                                  const RazeReleaseInfo& release_info,
                                                  std::vector<RazePackageUpdate>* package_updates);

/** Download a release package asset to destination_file. */
STMANAGER_EXPORT Status download_raze_package(const std::string& download_url,
                                              const std::string& destination_file);

/** Compute a lowercase hex SHA-256 digest for file_path. */
STMANAGER_EXPORT Status compute_file_sha256(const std::string& file_path, std::string* hash_hex);

/** Verify a downloaded package file against version.json hash metadata. */
STMANAGER_EXPORT Status verify_raze_package_file(const std::string& file_path,
                                                 const std::string& hash_algorithm,
                                                 const std::string& expected_hash);

/** Extract a zip/tar package archive into root_path with path traversal checks. */
STMANAGER_EXPORT Status extract_raze_package(const std::string& archive_file,
                                             const std::string& root_path);

}  // namespace STManager

#endif

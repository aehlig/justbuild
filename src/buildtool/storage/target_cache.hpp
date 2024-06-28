// Copyright 2022 Huawei Cloud Computing Technology Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef INCLUDED_SRC_BUILDTOOL_STORAGE_TARGET_CACHE_HPP
#define INCLUDED_SRC_BUILDTOOL_STORAGE_TARGET_CACHE_HPP

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gsl/gsl"
#include "src/buildtool/build_engine/base_maps/entity_name_data.hpp"
#include "src/buildtool/build_engine/expression/configuration.hpp"
#include "src/buildtool/common/artifact.hpp"
#include "src/buildtool/execution_api/remote/config.hpp"
#include "src/buildtool/file_system/file_storage.hpp"
#include "src/buildtool/file_system/object_type.hpp"
#include "src/buildtool/logging/logger.hpp"
#include "src/buildtool/storage/config.hpp"
#include "src/buildtool/storage/local_cas.hpp"
#include "src/buildtool/storage/target_cache_entry.hpp"
#include "src/buildtool/storage/target_cache_key.hpp"
#include "src/utils/cpp/gsl.hpp"

/// \brief The high-level target cache for storing export target's data.
/// Supports global uplinking across all generations using the garbage
/// collector. The uplink is automatically performed for every entry that is
/// read and already exists in an older generation.
/// \tparam kDoGlobalUplink     Enable global uplinking via garbage collector.
template <bool kDoGlobalUplink>
class TargetCache {
  public:
    /// Local target cache generation used by GC without global uplink.
    using LocalGenerationTC = TargetCache</*kDoGlobalUplink=*/false>;

    /// Callback type for downloading known artifacts to local CAS.
    using ArtifactDownloader =
        std::function<bool(std::vector<Artifact::ObjectInfo> const&)>;

    explicit TargetCache(
        gsl::not_null<LocalCAS<kDoGlobalUplink> const*> const& cas,
        GenerationConfig const& config)
        : cas_{*cas},
          file_store_{config.target_cache / ComputeShard()},
          explicit_shard_{std::nullopt} {
        if constexpr (kDoGlobalUplink) {
            // write backend description (shard) to CAS
            [[maybe_unused]] auto id =
                cas_.StoreBlob(RemoteExecutionConfig::DescribeBackend());
            EnsuresAudit(id and ArtifactDigest{*id}.hash() == ComputeShard());
        }
    }

    /// \brief Returns a new TargetCache backed by the same CAS, but the
    /// FileStorage uses the given \p shard. This is particularly useful for the
    /// just-serve server implementation, since the sharding must be performed
    /// according to the client's request and not following the server
    /// configuration. It is caller's responsibility to check that \p shard is a
    /// valid path.
    [[nodiscard]] auto WithShard(const std::optional<std::string>& shard) const
        -> TargetCache {
        return shard ? TargetCache<kDoGlobalUplink>(*this, *shard) : *this;
    }

    TargetCache(TargetCache const&) = default;
    TargetCache(TargetCache&&) noexcept = default;
    auto operator=(TargetCache const&) -> TargetCache& = default;
    auto operator=(TargetCache&&) noexcept -> TargetCache& = default;
    ~TargetCache() noexcept = default;

    /// \brief Store new key-entry pair in the target cache.
    /// \param key          The target-cache key.
    /// \param value        The target-cache value to store.
    /// \param downloader   Callback for obtaining known artifacts to local CAS.
    /// \returns true on success.
    [[nodiscard]] auto Store(
        TargetCacheKey const& key,
        TargetCacheEntry const& value,
        ArtifactDownloader const& downloader) const noexcept -> bool;

    /// \brief Calculate TargetCacheKey based on auxiliary information.
    /// Doesn't create a TargetCacheEntry in the TargetCache.
    /// \return TargetCacheKey on success.
    [[nodiscard]] auto ComputeKey(
        std::string const& repo_key,
        BuildMaps::Base::NamedTarget const& target_name,
        Configuration const& effective_config) const noexcept
        -> std::optional<TargetCacheKey>;

    /// \brief Read existing entry and object info from the target cache.
    /// \param key  The target-cache key to read the entry from.
    /// \param shard Optional explicit shard, if the default is not intended.
    /// \returns Pair of cache entry and its object info on success or nullopt.
    [[nodiscard]] auto Read(TargetCacheKey const& key) const noexcept
        -> std::optional<std::pair<TargetCacheEntry, Artifact::ObjectInfo>>;

    /// \brief Uplink entry from this to latest target cache generation.
    /// This function is only available for instances that are used as local GC
    /// generations (i.e., disabled global uplink).
    /// \tparam kIsLocalGeneration  True if this instance is a local generation.
    /// \param latest   The latest target cache generation.
    /// \param key      The target-cache key for the entry to uplink.
    /// \returns True if entry was successfully uplinked.
    template <bool kIsLocalGeneration = not kDoGlobalUplink>
    requires(kIsLocalGeneration) [[nodiscard]] auto LocalUplinkEntry(
        LocalGenerationTC const& latest,
        TargetCacheKey const& key) const noexcept -> bool;

  private:
    // By default, overwrite existing entries. Unless this is a generation
    // (disabled global uplink), then we never want to overwrite any entries.
    static constexpr auto kStoreMode =
        kDoGlobalUplink ? StoreMode::LastWins : StoreMode::FirstWins;

    std::shared_ptr<Logger> logger_{std::make_shared<Logger>("TargetCache")};
    LocalCAS<kDoGlobalUplink> const& cas_;
    FileStorage<ObjectType::File,
                kStoreMode,
                /*kSetEpochTime=*/false>
        file_store_;
    std::optional<std::string> explicit_shard_{std::nullopt};

    explicit TargetCache(TargetCache const& other,
                         std::string const& explicit_shard)
        : cas_{other.cas_},
          file_store_{other.file_store_.StorageRoot().parent_path() /
                      explicit_shard},
          explicit_shard_{explicit_shard} {}

    template <bool kIsLocalGeneration = not kDoGlobalUplink>
    requires(kIsLocalGeneration) [[nodiscard]] auto LocalUplinkEntry(
        LocalGenerationTC const& latest,
        std::string const& key_digest) const noexcept -> bool;

    [[nodiscard]] static auto ComputeShard() noexcept -> std::string {
        return ArtifactDigest::Create<ObjectType::File>(
                   RemoteExecutionConfig::DescribeBackend())
            .hash();
    }

    [[nodiscard]] auto DownloadKnownArtifacts(
        TargetCacheEntry const& value,
        ArtifactDownloader const& downloader) const noexcept -> bool;
};

#ifdef BOOTSTRAP_BUILD_TOOL
using ActiveTargetCache = TargetCache<false>;
#else
// TargetCache type aware of bootstrapping
using ActiveTargetCache = TargetCache<true>;
#endif  // BOOTSTRAP_BUILD_TOOL

#include "src/buildtool/storage/target_cache.tpp"

namespace std {
template <>
struct hash<TargetCacheKey> {
    [[nodiscard]] auto operator()(TargetCacheKey const& k) const {
        return std::hash<Artifact::ObjectInfo>{}(k.Id());
    }
};
}  // namespace std

#endif  // INCLUDED_SRC_BUILDTOOL_STORAGE_TARGET_CACHE_HPP

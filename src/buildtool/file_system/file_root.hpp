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

#ifndef INCLUDED_SRC_BUILDTOOL_FILE_SYSTEM_FILE_ROOT_HPP
#define INCLUDED_SRC_BUILDTOOL_FILE_SYSTEM_FILE_ROOT_HPP

#include <algorithm>
#include <compare>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

#include "fmt/core.h"
#include "gsl/gsl"
#include "nlohmann/json.hpp"
#include "src/buildtool/common/artifact_description.hpp"
#include "src/buildtool/common/artifact_digest.hpp"
#include "src/buildtool/common/artifact_digest_factory.hpp"
#include "src/buildtool/common/git_hashes_converter.hpp"
#include "src/buildtool/common/protocol_traits.hpp"
#include "src/buildtool/crypto/hash_function.hpp"
#include "src/buildtool/file_system/file_system_manager.hpp"
#include "src/buildtool/file_system/git_cas.hpp"
#include "src/buildtool/file_system/git_tree.hpp"
#include "src/buildtool/file_system/object_type.hpp"
#include "src/buildtool/logging/log_level.hpp"
#include "src/buildtool/logging/logger.hpp"
#include "src/utils/cpp/concepts.hpp"
#include "src/utils/cpp/expected.hpp"
#include "src/utils/cpp/hash_combine.hpp"
// Keep it to ensure fmt::format works on JSON objects
#include "src/utils/cpp/json.hpp"  // IWYU pragma: keep

/// FilteredIterator is an helper class to allow for iteration over
/// directory-only or file-only entries stored inside the class
/// DirectoryEntries. Internally, DirectoryEntries holds a
/// map<string,ObjectType> or map<string, GitTree*>. While iterating, we are
/// just interested in the name of the entry (i.e., the string).
/// To decide which entries retain, the FilteredIterator requires a predicate.
template <StrMapConstForwardIterator I>
// I is a forward iterator
// I::value_type is a std::pair<const std::string, *>
class FilteredIterator {
  public:
    using value_type = std::string const;
    using pointer = value_type*;
    using reference = value_type&;
    using difference_type = std::ptrdiff_t;
    using iteratori_category = std::forward_iterator_tag;
    using predicate_t = std::function<bool(typename I::reference)>;

    FilteredIterator() noexcept = default;
    // [first, last) is a valid sequence
    FilteredIterator(I first, I last, predicate_t p) noexcept
        : iterator_{std::find_if(first, last, p)},
          end_{std::move(last)},
          p_{std::move(p)} {}

    auto operator*() const noexcept -> reference { return iterator_->first; }

    auto operator++() noexcept -> FilteredIterator& {
        ++iterator_;
        iterator_ = std::find_if(iterator_, end_, p_);
        return *this;
    }

    [[nodiscard]] auto begin() noexcept -> FilteredIterator& { return *this; }
    [[nodiscard]] auto end() const noexcept -> FilteredIterator {
        return FilteredIterator{end_, end_, p_};
    }

    [[nodiscard]] friend auto operator==(FilteredIterator const& x,
                                         FilteredIterator const& y) noexcept
        -> bool {
        return x.iterator_ == y.iterator_;
    }

    [[nodiscard]] friend auto operator!=(FilteredIterator const& x,
                                         FilteredIterator const& y) noexcept
        -> bool {
        return not(x == y);
    }

  private:
    I iterator_{};
    const I end_{};
    predicate_t p_{};
};

class FileRoot {
    using fs_root_t = std::filesystem::path;
    struct RootGit {
        gsl::not_null<GitCASPtr> cas;
        gsl::not_null<GitTreePtr> tree;
    };

  public:
    struct ComputedRoot {
        std::string repository;
        std::string target_module;
        std::string target_name;
        nlohmann::json config;

        [[nodiscard]] auto operator==(
            ComputedRoot const& other) const noexcept {
            return (repository == other.repository) and
                   (target_module == other.target_module) and
                   (target_name == other.target_name) and
                   (config == other.config);
        }

        [[nodiscard]] auto operator<(ComputedRoot const& other) const noexcept {
            if (auto const res = repository <=> other.repository; res != 0) {
                return res < 0;
            }
            if (auto const res = target_module <=> other.target_module;
                res != 0) {
                return res < 0;
            }
            if (auto const res = target_name <=> other.target_name; res != 0) {
                return res < 0;
            }
            return config < other.config;
        }

        [[nodiscard]] auto ToString() const -> std::string {
            return fmt::format("([\"@\", {}, {}, {}], {})",
                               nlohmann::json(repository).dump(),
                               nlohmann::json(target_module).dump(),
                               nlohmann::json(target_name).dump(),
                               config.dump());
        }
    };

  private:
    // absent roots are defined by a tree hash with no witnessing repository
    using absent_root_t = std::string;
    using root_t =
        std::variant<fs_root_t, RootGit, absent_root_t, ComputedRoot>;

  public:
    static constexpr auto kGitTreeMarker = "git tree";
    static constexpr auto kGitTreeIgnoreSpecialMarker =
        "git tree ignore-special";
    static constexpr auto kFileIgnoreSpecialMarker = "file ignore-special";
    static constexpr auto kComputedMarker = "computed";

    class DirectoryEntries {
        friend class FileRoot;

      public:
        using pairs_t = std::unordered_map<std::string, ObjectType>;
        using tree_t = gsl::not_null<GitTree const*>;
        using entries_t = std::variant<tree_t, pairs_t>;

        using fs_iterator_type = typename pairs_t::const_iterator;
        using fs_iterator = FilteredIterator<fs_iterator_type>;

        using git_iterator_type = GitTree::entries_t::const_iterator;
        using git_iterator = FilteredIterator<git_iterator_type>;

      private:
        /// Iterator has two FilteredIterators, one for iterating over pairs_t
        /// and one for tree_t. Each FilteredIterator is constructed with a
        /// proper predicate, allowing for iteration on file-only or
        /// directory-only entries
        class Iterator {
          public:
            using value_type = std::string const;
            using pointer = value_type*;
            using reference = value_type&;
            using difference_type = std::ptrdiff_t;
            using iteratori_category = std::forward_iterator_tag;
            explicit Iterator(fs_iterator fs_it) : it_{std::move(fs_it)} {}
            explicit Iterator(git_iterator git_it) : it_{std::move(git_it)} {}

            auto operator*() const noexcept -> reference {
                if (std::holds_alternative<fs_iterator>(it_)) {
                    return *std::get<fs_iterator>(it_);
                }
                return *std::get<git_iterator>(it_);
            }

            [[nodiscard]] auto begin() noexcept -> Iterator& { return *this; }
            [[nodiscard]] auto end() const noexcept -> Iterator {
                if (std::holds_alternative<fs_iterator>(it_)) {
                    return Iterator{std::get<fs_iterator>(it_).end()};
                }
                return Iterator{std::get<git_iterator>(it_).end()};
            }
            auto operator++() noexcept -> Iterator& {
                if (std::holds_alternative<fs_iterator>(it_)) {
                    ++(std::get<fs_iterator>(it_));
                }
                else {
                    ++(std::get<git_iterator>(it_));
                }
                return *this;
            }

            friend auto operator==(Iterator const& x,
                                   Iterator const& y) noexcept -> bool {
                return x.it_ == y.it_;
            }
            friend auto operator!=(Iterator const& x,
                                   Iterator const& y) noexcept -> bool {
                return not(x == y);
            }

          private:
            std::variant<fs_iterator, git_iterator> it_;
        };

      public:
        explicit DirectoryEntries(pairs_t pairs) noexcept
            : data_{std::move(pairs)} {}

        explicit DirectoryEntries(tree_t const& git_tree) noexcept
            : data_{git_tree} {}

        [[nodiscard]] auto ContainsBlob(std::string const& name) const noexcept
            -> bool {
            if (auto const* const data = std::get_if<tree_t>(&data_)) {
                auto const ptr = (*data)->LookupEntryByName(name);
                return ptr != nullptr and IsBlobObject(ptr->Type());
            }
            if (auto const* const data = std::get_if<pairs_t>(&data_)) {
                auto const it = data->find(name);
                return it != data->end() and IsBlobObject(it->second);
            }
            return false;
        }

        [[nodiscard]] auto Empty() const noexcept -> bool {
            if (std::holds_alternative<tree_t>(data_)) {
                try {
                    auto const& tree = std::get<tree_t>(data_);
                    return tree->begin() == tree->end();
                } catch (...) {
                    return false;
                }
            }
            if (std::holds_alternative<pairs_t>(data_)) {
                return std::get<pairs_t>(data_).empty();
            }
            return true;
        }

        /// \brief Retrieve a root tree as a KNOWN artifact.
        /// Only succeeds if no entries have to be ignored.
        [[nodiscard]] auto AsKnownTree(HashFunction::Type hash_type,
                                       std::string const& repository)
            const noexcept -> std::optional<ArtifactDescription> {
            if (not ProtocolTraits::IsNative(hash_type)) {
                return std::nullopt;
            }
            if (std::holds_alternative<tree_t>(data_)) {
                try {
                    auto const& data = std::get<tree_t>(data_);
                    // only consider tree if we have it unmodified
                    if (auto id = data->Hash()) {
                        auto const& size = data->Size();
                        if (size) {
                            auto digest = ArtifactDigestFactory::Create(
                                HashFunction::Type::GitSHA1,
                                *id,
                                *size,
                                /*is_tree=*/true);
                            if (not digest) {
                                return std::nullopt;
                            }
                            return ArtifactDescription::CreateKnown(
                                *std::move(digest),
                                ObjectType::Tree,
                                repository);
                        }
                    }
                } catch (...) {
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] auto FilesIterator() const -> Iterator {
            if (std::holds_alternative<pairs_t>(data_)) {
                auto const& data = std::get<pairs_t>(data_);
                return Iterator{FilteredIterator{
                    data.begin(), data.end(), [](auto const& x) {
                        return IsFileObject(x.second);
                    }}};
            }
            // std::holds_alternative<tree_t>(data_) == true
            auto const& data = std::get<tree_t>(data_);
            return Iterator{FilteredIterator{
                data->begin(), data->end(), [](auto const& x) noexcept -> bool {
                    return IsFileObject(x.second->Type());
                }}};
        }

        [[nodiscard]] auto SymlinksIterator() const -> Iterator {
            if (std::holds_alternative<pairs_t>(data_)) {
                auto const& data = std::get<pairs_t>(data_);
                return Iterator{FilteredIterator{
                    data.begin(), data.end(), [](auto const& x) {
                        return IsSymlinkObject(x.second);
                    }}};
            }
            // std::holds_alternative<tree_t>(data_) == true
            auto const& data = std::get<tree_t>(data_);
            return Iterator{FilteredIterator{
                data->begin(), data->end(), [](auto const& x) noexcept -> bool {
                    return IsSymlinkObject(x.second->Type());
                }}};
        }

        [[nodiscard]] auto DirectoriesIterator() const -> Iterator {
            if (std::holds_alternative<pairs_t>(data_)) {
                auto const& data = std::get<pairs_t>(data_);
                return Iterator{FilteredIterator{
                    data.begin(), data.end(), [](auto const& x) {
                        return IsTreeObject(x.second);
                    }}};
            }

            // std::holds_alternative<tree_t>(data_) == true
            auto const& data = std::get<tree_t>(data_);
            return Iterator{FilteredIterator{
                data->begin(), data->end(), [](auto const& x) noexcept -> bool {
                    return x.second->IsTree();
                }}};
        }

      private:
        entries_t data_;
    };

    FileRoot() noexcept = default;
    explicit FileRoot(bool ignore_special) noexcept
        : ignore_special_(ignore_special) {}
    // avoid type narrowing errors, but force explicit choice of underlying type
    explicit FileRoot(char const* /*root*/) noexcept {
        Logger::Log(LogLevel::Error,
                    "FileRoot object instantiation must be explicit!");
    }
    explicit FileRoot(char const* /*root*/, bool /*ignore_special*/) noexcept {
        Logger::Log(LogLevel::Error,
                    "FileRoot object instantiation must be explicit!");
    }
    explicit FileRoot(std::string root) noexcept
        : root_{absent_root_t{std::move(root)}} {}
    explicit FileRoot(std::string root, bool ignore_special) noexcept
        : root_{absent_root_t{std::move(root)}},
          ignore_special_{ignore_special} {}
    explicit FileRoot(std::filesystem::path root) noexcept
        : root_{fs_root_t{std::move(root)}} {}
    FileRoot(std::filesystem::path root, bool ignore_special) noexcept
        : root_{fs_root_t{std::move(root)}}, ignore_special_{ignore_special} {}
    FileRoot(gsl::not_null<GitCASPtr> const& cas,
             gsl::not_null<GitTreePtr> const& tree,
             bool ignore_special = false) noexcept
        : root_{RootGit{cas, tree}}, ignore_special_{ignore_special} {}
    FileRoot(std::string repository,
             std::string target_module,
             std::string target_name,
             nlohmann::json config) noexcept
        : root_{ComputedRoot{std::move(repository),
                             std::move(target_module),
                             std::move(target_name),
                             std::move(config)}} {}

    [[nodiscard]] static auto FromGit(std::filesystem::path const& repo_path,
                                      std::string const& git_tree_id,
                                      bool ignore_special = false) noexcept
        -> std::optional<FileRoot> {
        auto cas = GitCAS::Open(repo_path);
        if (not cas) {
            return std::nullopt;
        }
        auto tree = GitTree::Read(cas, git_tree_id, ignore_special);
        if (not tree) {
            return std::nullopt;
        }
        try {
            return FileRoot{cas,
                            std::make_shared<GitTree const>(std::move(*tree)),
                            ignore_special};
        } catch (...) {
            return std::nullopt;
        }
    }

    /// \brief Return a complete description of the content of this root, if
    /// content fixed. This includes absent roots and any git-tree-based
    /// ignore-special roots.
    [[nodiscard]] auto ContentDescription() const noexcept
        -> std::optional<nlohmann::json> {
        try {
            if (std::holds_alternative<RootGit>(root_)) {
                nlohmann::json j;
                j.push_back(ignore_special_ ? kGitTreeIgnoreSpecialMarker
                                            : kGitTreeMarker);
                // we need the root tree id, irrespective of ignore_special flag
                j.push_back(std::get<RootGit>(root_).tree->FileRootHash());
                return j;
            }
            if (std::holds_alternative<absent_root_t>(root_)) {
                nlohmann::json j;
                j.push_back(ignore_special_ ? kGitTreeIgnoreSpecialMarker
                                            : kGitTreeMarker);
                j.push_back(std::get<absent_root_t>(root_));
                return j;
            }
        } catch (std::exception const& ex) {
            Logger::Log(LogLevel::Debug,
                        "Retrieving the description of a content-fixed root "
                        "failed unexpectedly with:\n{}",
                        ex.what());
        }
        return std::nullopt;
    }

    // Indicates that subsequent calls to `Exists()`, `IsFile()`,
    // `IsDirectory()`, and `BlobType()` on contents of the same directory will
    // be served without any additional file system lookups.
    [[nodiscard]] auto HasFastDirectoryLookup() const noexcept -> bool {
        return std::holds_alternative<RootGit>(root_);
    }

    [[nodiscard]] auto Exists(std::filesystem::path const& path) const noexcept
        -> bool {
        if (std::holds_alternative<RootGit>(root_)) {
            if (path == ".") {
                return true;
            }
            return static_cast<bool>(
                std::get<RootGit>(root_).tree->LookupEntryByPath(path));
        }
        if (std::holds_alternative<fs_root_t>(root_)) {
            auto root_path = std::get<fs_root_t>(root_) / path;
            auto exists = FileSystemManager::Exists(root_path);
            auto type =
                FileSystemManager::Type(root_path, /*allow_upwards=*/true);
            return (ignore_special_
                        ? exists and type and IsNonSpecialObject(*type)
                        : exists);
        }
        return false;  // absent roots cannot be interrogated locally
    }

    [[nodiscard]] auto IsFile(
        std::filesystem::path const& file_path) const noexcept -> bool {
        if (std::holds_alternative<RootGit>(root_)) {
            if (auto entry = std::get<RootGit>(root_).tree->LookupEntryByPath(
                    file_path)) {
                return IsFileObject(entry->Type());
            }
        }
        else if (std::holds_alternative<fs_root_t>(root_)) {
            return FileSystemManager::IsFile(std::get<fs_root_t>(root_) /
                                             file_path);
        }
        return false;  // absent roots cannot be interrogated locally
    }

    [[nodiscard]] auto IsSymlink(
        std::filesystem::path const& file_path) const noexcept -> bool {
        if (std::holds_alternative<RootGit>(root_)) {
            if (auto entry = std::get<RootGit>(root_).tree->LookupEntryByPath(
                    file_path)) {
                return IsSymlinkObject(entry->Type());
            }
        }
        else if (std::holds_alternative<fs_root_t>(root_)) {
            return FileSystemManager::IsNonUpwardsSymlink(
                std::get<fs_root_t>(root_) / file_path);
        }
        return false;  // absent roots cannot be interrogated locally
    }

    [[nodiscard]] auto IsBlob(
        std::filesystem::path const& file_path) const noexcept -> bool {
        return IsFile(file_path) or IsSymlink(file_path);
    }

    [[nodiscard]] auto IsDirectory(
        std::filesystem::path const& dir_path) const noexcept -> bool {
        if (std::holds_alternative<RootGit>(root_)) {
            if (dir_path == ".") {
                return true;
            }
            if (auto entry = std::get<RootGit>(root_).tree->LookupEntryByPath(
                    dir_path)) {
                return entry->IsTree();
            }
        }
        else if (std::holds_alternative<fs_root_t>(root_)) {
            return FileSystemManager::IsDirectory(std::get<fs_root_t>(root_) /
                                                  dir_path);
        }
        return false;  // absent roots cannot be interrogated locally
    }

    /// \brief Read content of file or symlink.
    [[nodiscard]] auto ReadContent(std::filesystem::path const& file_path)
        const noexcept -> std::optional<std::string> {
        if (std::holds_alternative<RootGit>(root_)) {
            if (auto entry = std::get<RootGit>(root_).tree->LookupEntryByPath(
                    file_path)) {
                if (IsBlobObject(entry->Type())) {
                    return entry->Blob();
                }
            }
        }
        else if (std::holds_alternative<fs_root_t>(root_)) {
            auto full_path = std::get<fs_root_t>(root_) / file_path;
            if (auto type = FileSystemManager::Type(full_path,
                                                    /*allow_upwards=*/true)) {
                return IsSymlinkObject(*type)
                           ? FileSystemManager::ReadSymlink(full_path)
                           : FileSystemManager::ReadFile(full_path);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto ReadDirectory(std::filesystem::path const& dir_path)
        const noexcept -> DirectoryEntries {
        try {
            if (std::holds_alternative<RootGit>(root_)) {
                auto const& tree = std::get<RootGit>(root_).tree;
                if (dir_path == ".") {
                    return DirectoryEntries{&(*tree)};
                }
                if (auto entry = tree->LookupEntryByPath(dir_path)) {
                    if (auto const& found_tree = entry->Tree(ignore_special_)) {
                        return DirectoryEntries{&(*found_tree)};
                    }
                }
            }
            else if (std::holds_alternative<fs_root_t>(root_)) {
                DirectoryEntries::pairs_t map{};
                if (FileSystemManager::ReadDirectory(
                        std::get<fs_root_t>(root_) / dir_path,
                        [&map](const auto& name, auto type) {
                            map.emplace(name.string(), type);
                            return true;
                        },
                        /*allow_upwards=*/false,
                        ignore_special_)) {
                    return DirectoryEntries{std::move(map)};
                }
            }
        } catch (std::exception const& ex) {
            Logger::Log(LogLevel::Error,
                        "reading directory {} failed with:\n{}",
                        dir_path.string(),
                        ex.what());
        }
        return DirectoryEntries{DirectoryEntries::pairs_t{}};
    }

    [[nodiscard]] auto BlobType(std::filesystem::path const& file_path)
        const noexcept -> std::optional<ObjectType> {
        if (std::holds_alternative<RootGit>(root_)) {
            if (auto entry = std::get<RootGit>(root_).tree->LookupEntryByPath(
                    file_path)) {
                if (IsBlobObject(entry->Type())) {
                    return entry->Type();
                }
            }
            return std::nullopt;
        }
        if (std::holds_alternative<fs_root_t>(root_)) {
            auto type = FileSystemManager::Type(
                std::get<fs_root_t>(root_) / file_path, /*allow_upwards=*/true);
            if (type and IsBlobObject(*type)) {
                return type;
            }
        }
        return std::nullopt;
    }

    /// \brief Read a blob from the root based on its ID.
    [[nodiscard]] auto ReadBlob(std::string const& blob_id) const noexcept
        -> std::optional<std::string> {
        if (std::holds_alternative<RootGit>(root_)) {
            return std::get<RootGit>(root_).cas->ReadObject(blob_id,
                                                            /*is_hex_id=*/true);
        }
        return std::nullopt;
    }

    /// \brief Read a root tree based on its ID.
    /// This should include all valid entry types.
    [[nodiscard]] auto ReadTree(std::string const& tree_id) const noexcept
        -> std::optional<GitTree> {
        if (std::holds_alternative<RootGit>(root_)) {
            try {
                auto const& cas = std::get<RootGit>(root_).cas;
                return GitTree::Read(cas, tree_id);
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    // Create LOCAL or KNOWN artifact. Does not check existence for LOCAL.
    // `file_path` must reference a blob.
    [[nodiscard]] auto ToArtifactDescription(
        HashFunction::Type hash_type,
        std::filesystem::path const& file_path,
        std::string const& repository) const noexcept
        -> std::optional<ArtifactDescription> {
        if (std::holds_alternative<RootGit>(root_)) {
            if (auto entry = std::get<RootGit>(root_).tree->LookupEntryByPath(
                    file_path)) {
                if (entry->IsBlob()) {
                    if (not ProtocolTraits::IsNative(hash_type)) {
                        auto compatible_hash =
                            GitHashesConverter::Instance().RegisterGitEntry(
                                entry->Hash(), *entry->Blob(), repository);
                        auto digest =
                            ArtifactDigestFactory::Create(hash_type,
                                                          compatible_hash,
                                                          *entry->Size(),
                                                          /*is_tree=*/false);
                        if (not digest) {
                            return std::nullopt;
                        }
                        return ArtifactDescription::CreateKnown(
                            *std::move(digest), entry->Type());
                    }
                    auto digest =
                        ArtifactDigestFactory::Create(hash_type,
                                                      entry->Hash(),
                                                      *entry->Size(),
                                                      /*is_tree=*/false);
                    if (not digest) {
                        return std::nullopt;
                    }
                    return ArtifactDescription::CreateKnown(
                        *std::move(digest), entry->Type(), repository);
                }
            }
            return std::nullopt;
        }
        if (std::holds_alternative<fs_root_t>(root_)) {
            return ArtifactDescription::CreateLocal(file_path, repository);
        }
        return std::nullopt;  // absent roots are neither LOCAL nor KNOWN
    }

    [[nodiscard]] auto IsAbsent() const noexcept -> bool {
        return std::holds_alternative<absent_root_t>(root_);
    }

    [[nodiscard]] auto GetAbsentTreeId() const noexcept
        -> std::optional<std::string> {
        if (std::holds_alternative<absent_root_t>(root_)) {
            try {
                return std::get<absent_root_t>(root_);
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto IsComputed() const noexcept -> bool {
        return std::holds_alternative<ComputedRoot>(root_);
    }

    [[nodiscard]] auto GetComputedDescription() const noexcept
        -> std::optional<ComputedRoot> {
        if (std::holds_alternative<ComputedRoot>(root_)) {
            try {
                return std::get<ComputedRoot>(root_);
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto IgnoreSpecial() const noexcept -> bool {
        return ignore_special_;
    }

    /// \brief Parses a FileRoot from string. On errors, populates error_msg.
    /// \returns the FileRoot and optional local path (if the root is local),
    /// nullopt on errors.
    [[nodiscard]] static auto ParseRoot(std::string const& repo,
                                        std::string const& keyword,
                                        nlohmann::json const& root,
                                        gsl::not_null<std::string*> error_msg)
        -> std::optional<
            std::pair<FileRoot, std::optional<std::filesystem::path>>> {
        if ((not root.is_array()) or root.empty()) {
            *error_msg = fmt::format(
                "Expected {} for {} to be of the form [<scheme>, ...], but "
                "found {}",
                keyword,
                repo,
                root.dump());
            return std::nullopt;
        }
        if (root[0] == "file") {
            if (root.size() != 2 or (not root[1].is_string())) {
                *error_msg = fmt::format(
                    "\"file\" scheme expects precisely one string argument, "
                    "but found {} for {} of repository {}",
                    root.dump(),
                    keyword,
                    repo);
                return std::nullopt;
            }
            auto path = std::filesystem::path{root[1].get<std::string>()};
            return std::pair(FileRoot{path}, std::move(path));
        }
        if (root[0] == FileRoot::kGitTreeMarker) {
            bool const has_one_arg = root.size() == 2 and root[1].is_string();
            bool const has_two_args = root.size() == 3 and
                                      root[1].is_string() and
                                      root[2].is_string();
            if (not has_one_arg and not has_two_args) {
                *error_msg = fmt::format(
                    "\"git tree\" scheme expects one or two string "
                    "arguments, but found {} for {} of repository {}",
                    root.dump(),
                    keyword,
                    repo);
                return std::nullopt;
            }
            if (root.size() == 3) {
                if (auto git_root = FileRoot::FromGit(root[2], root[1])) {
                    return std::pair(std::move(*git_root), std::nullopt);
                }
                *error_msg = fmt::format(
                    "Could not create file root for {}tree id {}",
                    root.size() == 3
                        ? fmt::format("git repository {} and ", root[2])
                        : "",
                    root[1]);
                return std::nullopt;
            }
            // return absent root
            return std::pair(FileRoot{std::string{root[1]}}, std::nullopt);
        }
        if (root[0] == FileRoot::kFileIgnoreSpecialMarker) {
            if (root.size() != 2 or (not root[1].is_string())) {
                *error_msg = fmt::format(
                    "\"file ignore-special\" scheme expects precisely "
                    "one string "
                    "argument, but found {} for {} of repository {}",
                    root.dump(),
                    keyword,
                    repo);
                return std::nullopt;
            }
            auto path = std::filesystem::path{root[1].get<std::string>()};
            return std::pair(FileRoot{path, /*ignore_special=*/true},
                             std::move(path));
        }
        if (root[0] == FileRoot::kGitTreeIgnoreSpecialMarker) {
            bool const has_one_arg = root.size() == 2 and root[1].is_string();
            bool const has_two_args = root.size() == 3 and
                                      root[1].is_string() and
                                      root[2].is_string();
            if (not has_one_arg and not has_two_args) {
                *error_msg = fmt::format(
                    "\"git tree ignore-special\" scheme expects one or two "
                    "string arguments, but found {} for {} of repository {}",
                    root.dump(),
                    keyword,
                    repo);
                return std::nullopt;
            }
            if (root.size() == 3) {
                if (auto git_root = FileRoot::FromGit(
                        root[2], root[1], /*ignore_special=*/true)) {
                    return std::pair(std::move(*git_root), std::nullopt);
                }
                *error_msg = fmt::format(
                    "Could not create ignore-special file root for {}tree id "
                    "{}",
                    root.size() == 3
                        ? fmt::format("git repository {} and ", root[2])
                        : "",
                    root[1]);
                return std::nullopt;
            }
            // return absent root
            return std::pair(
                FileRoot{std::string{root[1]}, /*ignore_special=*/true},
                std::nullopt);
        }
        if (root[0] == FileRoot::kComputedMarker) {
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
            if (root.size() != 5 or (not root[1].is_string()) or
                (not root[2].is_string()) or (not root[3].is_string()) or
                (not root[4].is_object())) {
                *error_msg = fmt::format(
                    "{} scheme requires, in this order, the arugments root, "
                    "module, name, config. However found {} for {} of "
                    "repository {}",
                    kComputedMarker,
                    root.dump(),
                    keyword,
                    repo);
                return std::nullopt;
            }
            return std::pair(FileRoot{std::string{root[1]},
                                      std::string{root[2]},
                                      std::string{root[3]},
                                      root[4]},
                             std::nullopt);
        }
        *error_msg = fmt::format(
            "Unknown scheme in the specification {} of {} of repository {}",
            root.dump(),
            keyword,
            repo);
        return std::nullopt;
    }

  private:
    root_t root_;
    // If set, forces lookups to ignore entries which are neither file nor
    // directories instead of erroring out. This means implicitly also that
    // there are no more fast tree lookups, i.e., tree traversal is a must.
    bool ignore_special_{};
};

namespace std {
template <>
struct hash<FileRoot::ComputedRoot> {
    [[nodiscard]] auto operator()(FileRoot::ComputedRoot const& cr) const
        -> std::size_t {
        size_t seed{};
        hash_combine<std::string>(&seed, cr.repository);
        hash_combine<std::string>(&seed, cr.target_module);
        hash_combine<std::string>(&seed, cr.target_name);
        hash_combine<nlohmann::json>(&seed, cr.config);
        return seed;
    }
};
}  // namespace std

#endif  // INCLUDED_SRC_BUILDTOOL_FILE_SYSTEM_FILE_ROOT_HPP

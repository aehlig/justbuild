#include "src/buildtool/execution_api/local/local_action.hpp"

#include <algorithm>
#include <filesystem>

#include "gsl-lite/gsl-lite.hpp"
#include "src/buildtool/common/bazel_types.hpp"
#include "src/buildtool/execution_api/local/local_response.hpp"
#include "src/buildtool/file_system/file_system_manager.hpp"
#include "src/buildtool/file_system/object_type.hpp"
#include "src/buildtool/system/system_command.hpp"

namespace {

/// \brief Removes specified directory if KeepBuildDir() is not set.
class BuildCleanupAnchor {
  public:
    explicit BuildCleanupAnchor(std::filesystem::path build_path) noexcept
        : build_path{std::move(build_path)} {}
    BuildCleanupAnchor(BuildCleanupAnchor const&) = delete;
    BuildCleanupAnchor(BuildCleanupAnchor&&) = delete;
    auto operator=(BuildCleanupAnchor const&) -> BuildCleanupAnchor& = delete;
    auto operator=(BuildCleanupAnchor&&) -> BuildCleanupAnchor& = delete;
    ~BuildCleanupAnchor() {
        if (not LocalExecutionConfig::KeepBuildDir() and
            not FileSystemManager::RemoveDirectory(build_path, true)) {
            Logger::Log(LogLevel::Error,
                        "Could not cleanup build directory {}",
                        build_path.string());
        }
    }

  private:
    std::filesystem::path const build_path{};
};

}  // namespace

auto LocalAction::Execute(Logger const* logger) noexcept
    -> IExecutionResponse::Ptr {
    auto do_cache = CacheEnabled(cache_flag_);
    auto action = CreateActionDigest(root_digest_, not do_cache);

    if (logger != nullptr) {
        logger->Emit(LogLevel::Trace,
                     "start execution\n"
                     " - exec_dir digest: {}\n"
                     " - action digest: {}",
                     root_digest_.hash(),
                     action.hash());
    }

    if (do_cache) {
        if (auto result = storage_->CachedActionResult(action)) {
            if (result->exit_code() == 0) {
                return IExecutionResponse::Ptr{
                    new LocalResponse{action.hash(),
                                      {std::move(*result), /*is_cached=*/true},
                                      storage_}};
            }
        }
    }

    if (ExecutionEnabled(cache_flag_)) {
        if (auto output = Run(action)) {
            if (cache_flag_ == CacheFlag::PretendCached) {
                // ensure the same id is created as if caching were enabled
                auto action_id = CreateActionDigest(root_digest_, false).hash();
                output->is_cached = true;
                return IExecutionResponse::Ptr{new LocalResponse{
                    std::move(action_id), std::move(*output), storage_}};
            }
            return IExecutionResponse::Ptr{
                new LocalResponse{action.hash(), std::move(*output), storage_}};
        }
    }

    return nullptr;
}

auto LocalAction::Run(bazel_re::Digest const& action_id) const noexcept
    -> std::optional<Output> {
    auto exec_path = CreateUniquePath(LocalExecutionConfig::GetBuildDir() /
                                      "exec_root" / action_id.hash());

    if (not exec_path) {
        return std::nullopt;
    }

    // anchor for cleaning up build directory at end of function (using RAII)
    auto anchor = BuildCleanupAnchor(*exec_path);

    auto const build_root = *exec_path / "build_root";
    if (not CreateDirectoryStructure(build_root)) {
        return std::nullopt;
    }

    if (cmdline_.empty()) {
        logger_.Emit(LogLevel::Error, "malformed command line");
        return std::nullopt;
    }

    auto cmdline = LocalExecutionConfig::GetLauncher();
    std::copy(cmdline_.begin(), cmdline_.end(), std::back_inserter(cmdline));

    SystemCommand system{"LocalExecution"};
    auto const command_output =
        system.Execute(cmdline, env_vars_, build_root, *exec_path);
    if (command_output.has_value()) {
        Output result{};
        result.action.set_exit_code(command_output->return_value);
        if (gsl::owner<bazel_re::Digest*> digest_ptr =
                DigestFromOwnedFile(command_output->stdout_file)) {
            result.action.set_allocated_stdout_digest(digest_ptr);
        }
        if (gsl::owner<bazel_re::Digest*> digest_ptr =
                DigestFromOwnedFile(command_output->stderr_file)) {
            result.action.set_allocated_stderr_digest(digest_ptr);
        }

        if (CollectAndStoreOutputs(&result.action, build_root)) {
            if (cache_flag_ == CacheFlag::CacheOutput) {
                if (not storage_->StoreActionResult(action_id, result.action)) {
                    logger_.Emit(LogLevel::Warning,
                                 "failed to store action results");
                }
            }
        }
        return result;
    }

    logger_.Emit(LogLevel::Error, "failed to execute commands");

    return std::nullopt;
}

auto LocalAction::StageFile(std::filesystem::path const& target_path,
                            Artifact::ObjectInfo const& info) const -> bool {
    auto blob_path =
        storage_->BlobPath(info.digest, IsExecutableObject(info.type));

    return blob_path and
           FileSystemManager::CreateDirectory(target_path.parent_path()) and
           FileSystemManager::CreateFileHardlink(*blob_path, target_path);
}

auto LocalAction::StageInputFiles(
    std::filesystem::path const& exec_path) const noexcept -> bool {
    if (FileSystemManager::IsRelativePath(exec_path)) {
        return false;
    }

    auto infos = storage_->ReadTreeInfos(root_digest_, exec_path);
    if (not infos) {
        return false;
    }
    for (std::size_t i{}; i < infos->first.size(); ++i) {
        if (not StageFile(infos->first.at(i), infos->second.at(i))) {
            return false;
        }
    }
    return true;
}

auto LocalAction::CreateDirectoryStructure(
    std::filesystem::path const& exec_path) const noexcept -> bool {
    // clean execution directory
    if (not FileSystemManager::RemoveDirectory(exec_path, true)) {
        logger_.Emit(LogLevel::Error, "failed to clean exec_path");
        return false;
    }

    // create process-exclusive execution directory
    if (not FileSystemManager::CreateDirectoryExclusive(exec_path)) {
        logger_.Emit(LogLevel::Error, "failed to exclusively create exec_path");
        return false;
    }

    // stage input files to execution directory
    if (not StageInputFiles(exec_path)) {
        logger_.Emit(LogLevel::Error,
                     "failed to stage input files to exec_path");
        return false;
    }

    // create output paths
    for (auto const& local_path : output_files_) {
        if (not FileSystemManager::CreateDirectory(
                (exec_path / local_path).parent_path())) {
            logger_.Emit(LogLevel::Error, "failed to create output directory");
            return false;
        }
    }
    for (auto const& local_path : output_dirs_) {
        if (not FileSystemManager::CreateDirectory(exec_path / local_path)) {
            logger_.Emit(LogLevel::Error, "failed to create output directory");
            return false;
        }
    }

    return true;
}

auto LocalAction::CollectOutputFile(std::filesystem::path const& exec_path,
                                    std::string const& local_path)
    const noexcept -> std::optional<bazel_re::OutputFile> {
    auto file_path = exec_path / local_path;
    auto type = FileSystemManager::Type(file_path);
    if (not type or not IsFileObject(*type)) {
        Logger::Log(LogLevel::Error, "expected file at {}", local_path);
        return std::nullopt;
    }
    bool is_executable = IsExecutableObject(*type);
    auto digest =
        storage_->StoreBlob</*kOwner=*/true>(file_path, is_executable);
    if (digest) {
        auto out_file = bazel_re::OutputFile{};
        out_file.set_path(local_path);
        out_file.set_allocated_digest(
            gsl::owner<bazel_re::Digest*>{new bazel_re::Digest{*digest}});
        out_file.set_is_executable(is_executable);
        return out_file;
    }
    return std::nullopt;
}

auto LocalAction::CollectOutputDir(std::filesystem::path const& exec_path,
                                   std::string const& local_path) const noexcept
    -> std::optional<bazel_re::OutputDirectory> {
    auto dir_path = exec_path / local_path;
    auto type = FileSystemManager::Type(dir_path);
    if (not type or not IsTreeObject(*type)) {
        Logger::Log(LogLevel::Error, "expected directory at {}", local_path);
        return std::nullopt;
    }
    auto digest = BazelMsgFactory::CreateDirectoryDigestFromLocalTree(
        dir_path,
        [this](auto path, auto is_exec) {
            return storage_->StoreBlob</*kOwner=*/true>(path, is_exec);
        },
        [this](auto bytes, auto dir) -> std::optional<bazel_re::Digest> {
            auto digest = storage_->StoreBlob(bytes);
            if (digest and not tree_map_->HasTree(*digest)) {
                auto tree = tree_map_->CreateTree();
                if (not BazelMsgFactory::ReadObjectInfosFromDirectory(
                        dir,
                        [&tree](auto path, auto info) {
                            return tree.AddInfo(path, info);
                        }) or
                    not tree_map_->AddTree(*digest, std::move(tree))) {
                    return std::nullopt;
                }
            }
            return digest;
        });
    if (digest) {
        auto out_dir = bazel_re::OutputDirectory{};
        out_dir.set_path(local_path);
        out_dir.set_allocated_tree_digest(
            gsl::owner<bazel_re::Digest*>{new bazel_re::Digest{*digest}});
        return out_dir;
    }
    return std::nullopt;
}

auto LocalAction::CollectAndStoreOutputs(
    bazel_re::ActionResult* result,
    std::filesystem::path const& exec_path) const noexcept -> bool {
    logger_.Emit(LogLevel::Trace, "collecting outputs:");
    for (auto const& path : output_files_) {
        auto out_file = CollectOutputFile(exec_path, path);
        if (not out_file) {
            logger_.Emit(
                LogLevel::Error, "could not collect output file {}", path);
            return false;
        }
        auto const& digest = out_file->digest().hash();
        logger_.Emit(LogLevel::Trace, " - file {}: {}", path, digest);
        result->mutable_output_files()->Add(std::move(*out_file));
    }
    for (auto const& path : output_dirs_) {
        auto out_dir = CollectOutputDir(exec_path, path);
        if (not out_dir) {
            logger_.Emit(
                LogLevel::Error, "could not collect output dir {}", path);
            return false;
        }
        auto const& digest = out_dir->tree_digest().hash();
        logger_.Emit(LogLevel::Trace, " - dir {}: {}", path, digest);
        result->mutable_output_directories()->Add(std::move(*out_dir));
    }

    return true;
}

auto LocalAction::DigestFromOwnedFile(std::filesystem::path const& file_path)
    const noexcept -> gsl::owner<bazel_re::Digest*> {
    if (auto digest = storage_->StoreBlob</*kOwner=*/true>(file_path)) {
        return new bazel_re::Digest{std::move(*digest)};
    }
    return nullptr;
}

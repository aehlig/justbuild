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

#include "src/other_tools/root_maps/commit_git_map.hpp"

#include <algorithm>

#include "src/buildtool/file_system/file_root.hpp"
#include "src/other_tools/git_operations/git_repo_remote.hpp"
#include "src/other_tools/just_mr/progress_reporting/progress.hpp"
#include "src/other_tools/just_mr/progress_reporting/statistics.hpp"
#include "src/utils/cpp/tmp_dir.hpp"

namespace {

[[nodiscard]] auto GitURLIsPath(std::string const& url) noexcept -> bool {
    auto prefixes = std::vector<std::string>{"ssh://", "http://", "https://"};
    // return true if no URL prefix exists
    return std::none_of(
        prefixes.begin(), prefixes.end(), [url](auto const& prefix) {
            return (url.rfind(prefix, 0) == 0);
        });
}

void EnsureCommit(GitRepoInfo const& repo_info,
                  std::filesystem::path const& repo_root,
                  GitCASPtr const& git_cas,
                  gsl::not_null<CriticalGitOpMap*> const& critical_git_op_map,
                  gsl::not_null<ImportToGitMap*> const& import_to_git_map,
                  std::string const& git_bin,
                  std::vector<std::string> const& launcher,
                  ServeApi* serve_api,
                  IExecutionApi* local_api,
                  IExecutionApi* remote_api,
                  bool fetch_absent,
                  gsl::not_null<TaskSystem*> const& ts,
                  CommitGitMap::SetterPtr const& ws_setter,
                  CommitGitMap::LoggerPtr const& logger) {
    // ensure commit exists, and fetch if needed
    auto git_repo = GitRepoRemote::Open(git_cas);  // link fake repo to odb
    if (not git_repo) {
        (*logger)(
            fmt::format("Could not open repository {}", repo_root.string()),
            /*fatal=*/true);
        return;
    }
    // setup wrapped logger
    auto wrapped_logger = std::make_shared<AsyncMapConsumerLogger>(
        [logger](auto const& msg, bool fatal) {
            (*logger)(fmt::format("While checking commit exists:\n{}", msg),
                      fatal);
        });
    auto is_commit_present =
        git_repo->CheckCommitExists(repo_info.hash, wrapped_logger);
    if (is_commit_present == std::nullopt) {
        return;
    }
    if (not is_commit_present.value()) {
        JustMRProgress::Instance().TaskTracker().Start(repo_info.origin);
        // check if commit is known to remote serve service, if asked for an
        // absent root
        if (repo_info.absent) {
            if (serve_api != nullptr) {
                if (auto tree_id = serve_api->RetrieveTreeFromCommit(
                        repo_info.hash, repo_info.subdir, fetch_absent)) {
                    if (not fetch_absent) {
                        // set the workspace root as absent
                        JustMRProgress::Instance().TaskTracker().Stop(
                            repo_info.origin);
                        (*ws_setter)(std::pair(
                            nlohmann::json::array(
                                {repo_info.ignore_special
                                     ? FileRoot::kGitTreeIgnoreSpecialMarker
                                     : FileRoot::kGitTreeMarker,
                                 *tree_id}),
                            false));
                        return;
                    }
                    // verify if we know the tree already locally
                    auto wrapped_logger =
                        std::make_shared<AsyncMapConsumerLogger>(
                            [logger, tree = *tree_id](auto const& msg,
                                                      bool fatal) {
                                (*logger)(
                                    fmt::format("While verifying presence of "
                                                "tree {}:\n{}",
                                                tree,
                                                msg),
                                    fatal);
                            });
                    auto tree_present =
                        git_repo->CheckTreeExists(*tree_id, wrapped_logger);
                    if (not tree_present) {
                        return;
                    }
                    if (*tree_present) {
                        JustMRProgress::Instance().TaskTracker().Stop(
                            repo_info.origin);
                        (*ws_setter)(std::pair(
                            nlohmann::json::array(
                                {repo_info.ignore_special
                                     ? FileRoot::kGitTreeIgnoreSpecialMarker
                                     : FileRoot::kGitTreeMarker,
                                 *tree_id,
                                 StorageConfig::GitRoot().string()}),
                            false));
                        return;
                    }
                    // try to get the tree from remote execution endpoint
                    auto digest = ArtifactDigest{*tree_id, 0, /*is_tree=*/true};
                    if (remote_api != nullptr and local_api != nullptr and
                        remote_api->RetrieveToCas(
                            {Artifact::ObjectInfo{.digest = digest,
                                                  .type = ObjectType::Tree}},
                            local_api)) {
                        JustMRProgress::Instance().TaskTracker().Stop(
                            repo_info.origin);
                        // Move tree from CAS to local git storage
                        auto tmp_dir = JustMR::Utils::CreateTypedTmpDir(
                            "fetch-absent-root");
                        if (not tmp_dir) {
                            (*logger)(
                                fmt::format(
                                    "Failed to create tmp directory after "
                                    "fetching tree {} for absent commit {}",
                                    repo_info.hash,
                                    *tree_id),
                                true);
                            return;
                        }
                        if (not local_api->RetrieveToPaths(
                                {Artifact::ObjectInfo{
                                    .digest = digest,
                                    .type = ObjectType::Tree}},
                                {tmp_dir->GetPath()})) {
                            (*logger)(fmt::format("Failed to copy fetchted "
                                                  "tree {} to {}",
                                                  *tree_id,
                                                  tmp_dir->GetPath().string()),
                                      true);
                            return;
                        }
                        CommitInfo c_info{tmp_dir->GetPath(), "tree", *tree_id};
                        import_to_git_map->ConsumeAfterKeysReady(
                            ts,
                            {std::move(c_info)},
                            [tmp_dir,  // keep tmp_dir alive
                             repo_info,
                             tree_id,
                             logger,
                             ws_setter](auto const& values) {
                                if (not values[0]->second) {
                                    (*logger)("Importing to git failed",
                                              /*fatal=*/true);
                                    return;
                                }
                                // set the workspace root as present
                                (*ws_setter)(std::pair(
                                    nlohmann::json::array(
                                        {repo_info.ignore_special
                                             ? FileRoot::
                                                   kGitTreeIgnoreSpecialMarker
                                             : FileRoot::kGitTreeMarker,
                                         *tree_id,
                                         StorageConfig::GitRoot().string()}),
                                    false));
                            },
                            [logger, tmp_dir, tree_id](auto const& msg,
                                                       bool fatal) {
                                (*logger)(
                                    fmt::format("While moving tree {} from {} "
                                                "to local git:\n{}",
                                                *tree_id,
                                                tmp_dir->GetPath().string(),
                                                msg),
                                    fatal);
                            });

                        return;
                    }
                    // just serve should have made the tree available in the
                    // remote CAS, so log this attempt and revert to network
                    (*logger)(fmt::format("Tree {} marked as served, but not "
                                          "found on remote",
                                          *tree_id),
                              /*fatal=*/false);
                }
                else {
                    // give warning
                    (*logger)(
                        fmt::format("Tree at subdir {} for commit {} could "
                                    "not be served",
                                    repo_info.subdir,
                                    repo_info.hash),
                        /*fatal=*/false);
                }
            }
            else {
                if (not fetch_absent) {
                    // give warning
                    (*logger)(
                        "Absent root requested, but no serve endpoint provided",
                        /*fatal=*/false);
                }
            }
        }
        // default to fetching it from network
        auto tmp_dir = JustMR::Utils::CreateTypedTmpDir("fetch");
        if (not tmp_dir) {
            (*logger)("Failed to create fetch tmp directory!",
                      /*fatal=*/true);
            return;
        }
        // setup wrapped logger
        auto wrapped_logger = std::make_shared<AsyncMapConsumerLogger>(
            [logger](auto const& msg, bool fatal) {
                (*logger)(fmt::format("While fetching via tmp repo:\n{}", msg),
                          fatal);
            });
        if (not git_repo->FetchViaTmpRepo(tmp_dir->GetPath(),
                                          repo_info.repo_url,
                                          repo_info.branch,
                                          git_bin,
                                          launcher,
                                          wrapped_logger)) {
            return;
        }
        // setup wrapped logger
        wrapped_logger = std::make_shared<AsyncMapConsumerLogger>(
            [logger](auto const& msg, bool fatal) {
                (*logger)(fmt::format("While checking commit exists:\n{}", msg),
                          fatal);
            });
        // check if commit exists now, after fetch
        auto is_commit_present =
            git_repo->CheckCommitExists(repo_info.hash, wrapped_logger);
        if (not is_commit_present) {
            return;
        }
        if (not *is_commit_present) {
            // commit could not be fetched, so fail
            (*logger)(fmt::format("Could not fetch commit {} from branch "
                                  "{} for remote {}",
                                  repo_info.hash,
                                  repo_info.branch,
                                  repo_info.repo_url),
                      /*fatal=*/true);
            return;
        }
        // keep tag
        GitOpKey op_key = {.params =
                               {
                                   repo_root,                    // target_path
                                   repo_info.hash,               // git_hash
                                   "",                           // branch
                                   "Keep referenced tree alive"  // message
                               },
                           .op_type = GitOpType::KEEP_TAG};
        critical_git_op_map->ConsumeAfterKeysReady(
            ts,
            {std::move(op_key)},
            [git_cas, repo_info, repo_root, fetch_absent, ws_setter, logger](
                auto const& values) {
                GitOpValue op_result = *values[0];
                // check flag
                if (not op_result.result) {
                    (*logger)("Keep tag failed",
                              /*fatal=*/true);
                    return;
                }
                // ensure commit exists, and fetch if needed
                auto git_repo =
                    GitRepoRemote::Open(git_cas);  // link fake repo to odb
                if (not git_repo) {
                    (*logger)(fmt::format("Could not open repository {}",
                                          repo_root.string()),
                              /*fatal=*/true);
                    return;
                }
                // setup wrapped logger
                auto wrapped_logger = std::make_shared<AsyncMapConsumerLogger>(
                    [logger](auto const& msg, bool fatal) {
                        (*logger)(fmt::format("While getting subtree "
                                              "from commit:\n{}",
                                              msg),
                                  fatal);
                    });
                // get tree id and return workspace root
                auto subtree = git_repo->GetSubtreeFromCommit(
                    repo_info.hash, repo_info.subdir, wrapped_logger);
                if (not subtree) {
                    return;
                }
                // set the workspace root
                JustMRProgress::Instance().TaskTracker().Stop(repo_info.origin);
                auto root = nlohmann::json::array(
                    {repo_info.ignore_special
                         ? FileRoot::kGitTreeIgnoreSpecialMarker
                         : FileRoot::kGitTreeMarker,
                     *subtree});
                if (fetch_absent or not repo_info.absent) {
                    root.emplace_back(repo_root);
                }
                (*ws_setter)(std::pair(std::move(root), false));
            },
            [logger, target_path = repo_root](auto const& msg, bool fatal) {
                (*logger)(fmt::format("While running critical Git op "
                                      "KEEP_TAG for target {}:\n{}",
                                      target_path.string(),
                                      msg),
                          fatal);
            });
    }
    else {
        // setup wrapped logger
        auto wrapped_logger = std::make_shared<AsyncMapConsumerLogger>(
            [logger](auto const& msg, bool fatal) {
                (*logger)(
                    fmt::format("While getting subtree from commit:\n{}", msg),
                    fatal);
            });
        // get tree id and return workspace root
        auto subtree = git_repo->GetSubtreeFromCommit(
            repo_info.hash, repo_info.subdir, wrapped_logger);
        if (not subtree) {
            return;
        }
        // set the workspace root
        auto root = nlohmann::json::array(
            {repo_info.ignore_special ? FileRoot::kGitTreeIgnoreSpecialMarker
                                      : FileRoot::kGitTreeMarker,
             *subtree});
        if (fetch_absent or not repo_info.absent) {
            root.emplace_back(repo_root);
        }
        (*ws_setter)(std::pair(std::move(root), true));
    }
}

}  // namespace

/// \brief Create a CommitGitMap object
auto CreateCommitGitMap(
    gsl::not_null<CriticalGitOpMap*> const& critical_git_op_map,
    gsl::not_null<ImportToGitMap*> const& import_to_git_map,
    JustMR::PathsPtr const& just_mr_paths,
    std::string const& git_bin,
    std::vector<std::string> const& launcher,
    ServeApi* serve_api,
    IExecutionApi* local_api,
    IExecutionApi* remote_api,
    bool fetch_absent,
    std::size_t jobs) -> CommitGitMap {
    auto commit_to_git = [critical_git_op_map,
                          import_to_git_map,
                          just_mr_paths,
                          git_bin,
                          launcher,
                          serve_api,
                          local_api,
                          remote_api,
                          fetch_absent](auto ts,
                                        auto setter,
                                        auto logger,
                                        auto /* unused */,
                                        auto const& key) {
        // get root for repo (making sure that if repo is a path, it is
        // absolute)
        std::string fetch_repo = key.repo_url;
        if (GitURLIsPath(fetch_repo)) {
            fetch_repo = std::filesystem::absolute(fetch_repo).string();
        }
        std::filesystem::path repo_root =
            JustMR::Utils::GetGitRoot(just_mr_paths, fetch_repo);
        // ensure git repo
        // define Git operation to be done
        GitOpKey op_key = {
            .params =
                {
                    repo_root,     // target_path
                    "",            // git_hash
                    "",            // branch
                    std::nullopt,  // message
                    not just_mr_paths->git_checkout_locations.contains(
                        fetch_repo)  // init_bare
                },
            .op_type = GitOpType::ENSURE_INIT};
        critical_git_op_map->ConsumeAfterKeysReady(
            ts,
            {std::move(op_key)},
            [key,
             repo_root,
             critical_git_op_map,
             import_to_git_map,
             git_bin,
             launcher,
             serve_api,
             local_api,
             remote_api,
             fetch_absent,
             ts,
             setter,
             logger](auto const& values) {
                GitOpValue op_result = *values[0];
                // check flag
                if (not op_result.result) {
                    (*logger)("Git init failed",
                              /*fatal=*/true);
                    return;
                }
                // setup a wrapped_logger
                auto wrapped_logger = std::make_shared<AsyncMapConsumerLogger>(
                    [logger, target_path = repo_root](auto const& msg,
                                                      bool fatal) {
                        (*logger)(fmt::format("While ensuring commit for "
                                              "repository {}:\n{}",
                                              target_path.string(),
                                              msg),
                                  fatal);
                    });
                EnsureCommit(key,
                             repo_root,
                             op_result.git_cas,
                             critical_git_op_map,
                             import_to_git_map,
                             git_bin,
                             launcher,
                             serve_api,
                             local_api,
                             remote_api,
                             fetch_absent,
                             ts,
                             setter,
                             wrapped_logger);
            },
            [logger, target_path = repo_root](auto const& msg, bool fatal) {
                (*logger)(fmt::format("While running critical Git op "
                                      "ENSURE_INIT for target {}:\n{}",
                                      target_path.string(),
                                      msg),
                          fatal);
            });
    };
    return AsyncMapConsumer<GitRepoInfo, std::pair<nlohmann::json, bool>>(
        commit_to_git, jobs);
}

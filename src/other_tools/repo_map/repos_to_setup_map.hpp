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

#ifndef INCLUDED_SRC_OTHER_TOOLS_REPO_MAP_REPOS_TO_SETUP_MAP_HPP
#define INCLUDED_SRC_OTHER_TOOLS_REPO_MAP_REPOS_TO_SETUP_MAP_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "gsl/gsl"
#include "nlohmann/json.hpp"
#include "src/buildtool/build_engine/expression/configuration.hpp"
#include "src/buildtool/multithreading/async_map_consumer.hpp"
#include "src/other_tools/just_mr/progress_reporting/statistics.hpp"
#include "src/other_tools/root_maps/commit_git_map.hpp"
#include "src/other_tools/root_maps/content_git_map.hpp"
#include "src/other_tools/root_maps/distdir_git_map.hpp"
#include "src/other_tools/root_maps/foreign_file_git_map.hpp"
#include "src/other_tools/root_maps/fpath_git_map.hpp"
#include "src/other_tools/root_maps/tree_id_git_map.hpp"

/// \brief Maps a global repo name to a JSON object containing the workspace
/// root and the TAKE_OVER fields.
using ReposToSetupMap = AsyncMapConsumer<std::string, nlohmann::json>;

auto CreateReposToSetupMap(
    std::shared_ptr<Configuration> const& config,
    std::optional<std::string> const& main,
    bool interactive,
    gsl::not_null<CommitGitMap*> const& commit_git_map,
    gsl::not_null<ContentGitMap*> const& content_git_map,
    gsl::not_null<ForeignFileGitMap*> const& foreign_file_git_map,
    gsl::not_null<FilePathGitMap*> const& fpath_git_map,
    gsl::not_null<DistdirGitMap*> const& distdir_git_map,
    gsl::not_null<TreeIdGitMap*> const& tree_id_git_map,
    bool fetch_absent,
    gsl::not_null<JustMRStatistics*> const& stats,
    std::size_t jobs) -> ReposToSetupMap;

// use explicit cast to std::function to allow template deduction when used
static const std::function<std::string(std::string const&)>
    kReposToSetupPrinter =
        [](std::string const& x) -> std::string { return x; };

#endif  // INCLUDED_SRC_OTHER_TOOLS_REPO_MAP_REPOS_TO_SETUP_MAP_HPP

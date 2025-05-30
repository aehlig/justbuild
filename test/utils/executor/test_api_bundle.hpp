// Copyright 2024 Huawei Cloud Computing Technology Co., Ltd.
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

#ifndef INCLUDED_SRC_TEST_UTILS_EXECUTOR_TEST_API_BUNDLE_HPP
#define INCLUDED_SRC_TEST_UTILS_EXECUTOR_TEST_API_BUNDLE_HPP

#include "gsl/gsl"
#include "src/buildtool/execution_api/common/api_bundle.hpp"
#include "src/buildtool/execution_api/common/execution_api.hpp"

/// \brief Creates an ApiBundle that contains a given IExecutionApi
/// implementation.
[[nodiscard]] static auto CreateTestApiBundle(
    gsl::not_null<IExecutionApi::Ptr> const& api) noexcept -> ApiBundle {
    return ApiBundle{.local = api, .remote = api};
}

#endif  // INCLUDED_SRC_TEST_UTILS_EXECUTOR_TEST_API_BUNDLE_HPP

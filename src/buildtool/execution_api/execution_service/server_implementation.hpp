// Copyright 2023 Huawei Cloud Computing Technology Co., Ltd.
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

#ifndef SERVER_IMPLEMENATION_HPP
#define SERVER_IMPLEMENATION_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "gsl/gsl"
#include "src/buildtool/execution_api/local/context.hpp"
#include "src/buildtool/execution_api/remote/context.hpp"

class LocalApi;

class ServerImpl final {
  public:
    [[nodiscard]] static auto Create(
        std::optional<std::string> interface,
        std::optional<int> port,
        std::optional<std::string> info_file,
        std::optional<std::string> pid_file) noexcept
        -> std::optional<ServerImpl>;

    ~ServerImpl() noexcept = default;

    ServerImpl(ServerImpl const&) = delete;
    auto operator=(ServerImpl const&) noexcept -> ServerImpl& = delete;

    ServerImpl(ServerImpl&&) noexcept = default;
    auto operator=(ServerImpl&&) noexcept -> ServerImpl& = default;

    /// \brief Start the execution service.
    /// \param local_context    The LocalContext to be used.
    /// \param remote_context   The RemoteContext to be used.
    /// \param local_api        The LocalApi used.
    /// \param op_exponent      Log2 threshold for operation cache.
    auto Run(gsl::not_null<LocalContext const*> const& local_context,
             gsl::not_null<RemoteContext const*> const& remote_context,
             gsl::not_null<LocalApi const*> const& local_api,
             std::optional<std::uint8_t> op_exponent) -> bool;

  private:
    ServerImpl() noexcept = default;

    std::string interface_{"127.0.0.1"};
    int port_{0};
    std::string info_file_;
    std::string pid_file_;
};

#endif  // SERVER_IMPLEMENATION_HPP

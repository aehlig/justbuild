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

#include "src/buildtool/execution_api/execution_service/operations_server.hpp"

#include <optional>
#include <string>
#include <utility>

#include "fmt/core.h"
#include "src/buildtool/execution_api/execution_service/operation_cache.hpp"
#include "src/buildtool/logging/log_level.hpp"
#include "src/utils/cpp/hex_string.hpp"

auto OperationsServiceImpl::GetOperation(
    ::grpc::ServerContext* /*context*/,
    const ::google::longrunning::GetOperationRequest* request,
    ::google::longrunning::Operation* response) -> ::grpc::Status {
    auto const& hash = request->name();
    if (not IsHexString(hash)) {
        auto const str = fmt::format("Invalid hash {}", hash);
        logger_.Emit(LogLevel::Debug, "{}", str);
        return ::grpc::Status{::grpc::StatusCode::INVALID_ARGUMENT, str};
    }
    logger_.Emit(LogLevel::Trace, "GetOperation: {}", hash);
    auto op = op_cache_.Query(hash);
    if (not op) {
        auto const str = fmt::format(
            "Executing action {} not found in internal cache.", hash);
        logger_.Emit(LogLevel::Error, "{}", str);
        return ::grpc::Status{grpc::StatusCode::INTERNAL, str};
    }
    *response = *std::move(op);
    return ::grpc::Status::OK;
}

auto OperationsServiceImpl::ListOperations(
    ::grpc::ServerContext* /*context*/,
    const ::google::longrunning::ListOperationsRequest* /*request*/,
    ::google::longrunning::ListOperationsResponse* /*response*/)
    -> ::grpc::Status {
    static constexpr auto kStr = "ListOperations not implemented";
    logger_.Emit(LogLevel::Error, kStr);
    return ::grpc::Status{grpc::StatusCode::UNIMPLEMENTED, kStr};
}

auto OperationsServiceImpl::DeleteOperation(
    ::grpc::ServerContext* /*context*/,
    const ::google::longrunning::DeleteOperationRequest* /*request*/,
    ::google::protobuf::Empty* /*response*/) -> ::grpc::Status {
    static constexpr auto kStr = "DeleteOperation not implemented";
    logger_.Emit(LogLevel::Error, kStr);
    return ::grpc::Status{grpc::StatusCode::UNIMPLEMENTED, kStr};
}

auto OperationsServiceImpl::CancelOperation(
    ::grpc::ServerContext* /*context*/,
    const ::google::longrunning::CancelOperationRequest* /*request*/,
    ::google::protobuf::Empty* /*response*/) -> ::grpc::Status {
    static constexpr auto kStr = "CancelOperation not implemented";
    logger_.Emit(LogLevel::Error, kStr);
    return ::grpc::Status{grpc::StatusCode::UNIMPLEMENTED, kStr};
}

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

#include "src/buildtool/execution_api/execution_service/bytestream_server.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "fmt/core.h"
#include "google/protobuf/stubs/port.h"
#include "src/buildtool/common/artifact_digest.hpp"
#include "src/buildtool/crypto/hash_function.hpp"
#include "src/buildtool/execution_api/common/bytestream_utils.hpp"
#include "src/buildtool/execution_api/execution_service/cas_utils.hpp"
#include "src/buildtool/logging/log_level.hpp"
#include "src/buildtool/storage/garbage_collector.hpp"
#include "src/utils/cpp/expected.hpp"
#include "src/utils/cpp/incremental_reader.hpp"
#include "src/utils/cpp/tmp_dir.hpp"

auto BytestreamServiceImpl::Read(
    ::grpc::ServerContext* /*context*/,
    const ::google::bytestream::ReadRequest* request,
    ::grpc::ServerWriter<::google::bytestream::ReadResponse>* writer)
    -> ::grpc::Status {
    logger_.Emit(LogLevel::Trace, "Read {}", request->resource_name());
    auto const read_request =
        ByteStreamUtils::ReadRequest::FromString(request->resource_name());
    if (not read_request) {
        auto const str =
            fmt::format("could not parse {}", request->resource_name());
        logger_.Emit(LogLevel::Error, "{}", str);
        return ::grpc::Status{::grpc::StatusCode::INVALID_ARGUMENT, str};
    }
    auto const read_digest =
        read_request->GetDigest(storage_config_.hash_function.GetType());
    if (not read_digest) {
        logger_.Emit(LogLevel::Debug, "{}", read_digest.error());
        return ::grpc::Status{::grpc::StatusCode::INVALID_ARGUMENT,
                              read_digest.error()};
    }

    auto const lock = GarbageCollector::SharedLock(storage_config_);
    if (not lock) {
        static constexpr auto kStr = "Could not acquire SharedLock";
        logger_.Emit(LogLevel::Error, "{}", kStr);
        return grpc::Status{grpc::StatusCode::INTERNAL, kStr};
    }

    auto const path =
        read_digest->IsTree()
            ? storage_.CAS().TreePath(*read_digest)
            : storage_.CAS().BlobPath(*read_digest, /*is_executable=*/false);

    if (not path) {
        auto const str = fmt::format("could not find {}", read_digest->hash());
        logger_.Emit(LogLevel::Error, "{}", str);
        return ::grpc::Status{::grpc::StatusCode::NOT_FOUND, str};
    }

    auto const to_read =
        IncrementalReader::FromFile(ByteStreamUtils::kChunkSize, *path);
    if (not to_read.has_value()) {
        auto const str = fmt::format("Failed to create reader for {}:\n{}",
                                     read_digest->hash(),
                                     to_read.error());
        logger_.Emit(LogLevel::Error, str);
        return grpc::Status{grpc::StatusCode::INTERNAL, str};
    }

    ::google::bytestream::ReadResponse response;
    for (auto it = to_read->make_iterator(request->read_offset());
         it != to_read->end();
         ++it) {
        auto const chunk = *it;
        if (not chunk.has_value()) {
            auto const str = fmt::format("Failed to read data for {}:\n{}",
                                         read_digest->hash(),
                                         chunk.error());
            logger_.Emit(LogLevel::Error, str);
            return grpc::Status{grpc::StatusCode::INTERNAL, str};
        }
        *response.mutable_data() = *chunk;
        writer->Write(response);
    }
    return ::grpc::Status::OK;
}

auto BytestreamServiceImpl::Write(
    ::grpc::ServerContext* /*context*/,
    ::grpc::ServerReader<::google::bytestream::WriteRequest>* reader,
    ::google::bytestream::WriteResponse* response) -> ::grpc::Status {
    ::google::bytestream::WriteRequest request;
    reader->Read(&request);
    logger_.Emit(LogLevel::Debug, "write {}", request.resource_name());
    auto const write_request =
        ByteStreamUtils::WriteRequest::FromString(request.resource_name());
    if (not write_request) {
        auto const str =
            fmt::format("could not parse {}", request.resource_name());
        logger_.Emit(LogLevel::Error, "{}", str);
        return ::grpc::Status{::grpc::StatusCode::INVALID_ARGUMENT, str};
    }

    auto const write_digest =
        write_request->GetDigest(storage_config_.hash_function.GetType());
    if (not write_digest) {
        logger_.Emit(LogLevel::Debug, "{}", write_digest.error());
        return ::grpc::Status{::grpc::StatusCode::INVALID_ARGUMENT,
                              write_digest.error()};
    }
    logger_.Emit(LogLevel::Trace,
                 "Write: {}, offset {}, finish write {}",
                 write_digest->hash(),
                 request.write_offset(),
                 request.finish_write());
    auto const lock = GarbageCollector::SharedLock(storage_config_);
    if (not lock) {
        static constexpr auto kStr = "Could not acquire SharedLock";
        logger_.Emit(LogLevel::Error, "{}", kStr);
        return grpc::Status{grpc::StatusCode::INTERNAL, kStr};
    }
    auto const tmp_dir = storage_config_.CreateTypedTmpDir("execution-service");
    if (not tmp_dir) {
        return ::grpc::Status{::grpc::StatusCode::INTERNAL,
                              "could not create TmpDir"};
    }

    auto tmp = tmp_dir->GetPath() / write_digest->hash();
    {
        std::ofstream stream{tmp, std::ios::binary};
        do {  // NOLINT(cppcoreguidelines-avoid-do-while)
            if (not stream.good()) {
                auto const str = fmt::format("Failed to write data for {}",
                                             write_digest->hash());
                logger_.Emit(LogLevel::Error, "{}", str);
                return ::grpc::Status{::grpc::StatusCode::INTERNAL, str};
            }
            stream.write(request.data().data(),
                         static_cast<std::streamsize>(request.data().size()));
        } while (not request.finish_write() and reader->Read(&request));
    }

    auto const status = CASUtils::AddFileToCAS(*write_digest, tmp, storage_);
    if (not status.ok()) {
        auto const str = fmt::format("Write: {}", status.error_message());
        logger_.Emit(LogLevel::Error, "{}", str);
        return ::grpc::Status{status.error_code(), str};
    }
    response->set_committed_size(
        static_cast<google::protobuf::int64>(std::filesystem::file_size(tmp)));
    return ::grpc::Status::OK;
}

auto BytestreamServiceImpl::QueryWriteStatus(
    ::grpc::ServerContext* /*context*/,
    const ::google::bytestream::QueryWriteStatusRequest* /*request*/,
    ::google::bytestream::QueryWriteStatusResponse* /*response*/)
    -> ::grpc::Status {
    static constexpr auto kStr = "QueryWriteStatus not implemented";
    logger_.Emit(LogLevel::Error, "{}", kStr);
    return ::grpc::Status{grpc::StatusCode::UNIMPLEMENTED, kStr};
}

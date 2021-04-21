/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sys/types.h>
#include <unistd.h>
#include <string_view>
#include <thread>

#include <magic_enum.hpp>

#include "src/common/system/boot_clock.h"
#include "src/common/system/tcp_socket.h"
#include "src/common/system/udp_socket.h"
#include "src/shared/metadata/metadata.h"
#include "src/shared/types/column_wrapper.h"
#include "src/shared/types/types.h"
#include "src/stirling/core/data_table.h"
#include "src/stirling/core/output.h"
#include "src/stirling/source_connectors/socket_tracer/bcc_bpf_intf/socket_trace.hpp"
#include "src/stirling/source_connectors/socket_tracer/socket_trace_connector.h"
#include "src/stirling/source_connectors/socket_tracer/testing/client_server_system.h"
#include "src/stirling/source_connectors/socket_tracer/testing/socket_trace_bpf_test_fixture.h"
#include "src/stirling/testing/common.h"

namespace px {
namespace stirling {

using ::px::stirling::testing::ColWrapperIsEmpty;
using ::px::stirling::testing::ColWrapperSizeIs;
using ::px::stirling::testing::FindRecordIdxMatchesPID;
using ::px::stirling::testing::FindRecordsMatchingPID;
using ::px::system::TCPSocket;
using ::px::system::UDPSocket;
using ::px::types::ColumnWrapperRecordBatch;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::StrEq;

constexpr std::string_view kHTTPReqMsg1 = R"(GET /endpoint1 HTTP/1.1
User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:67.0) Gecko/20100101 Firefox/67.0

)";

constexpr std::string_view kHTTPReqMsg2 = R"(GET /endpoint2 HTTP/1.1
User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:67.0) Gecko/20100101 Firefox/67.0

)";

constexpr std::string_view kHTTPRespMsg1 = R"(HTTP/1.1 200 OK
Content-Type: application/json; msg1
Content-Length: 0

)";

constexpr std::string_view kHTTPRespMsg2 = R"(HTTP/1.1 200 OK
Content-Type: application/json; msg2
Content-Length: 0

)";

constexpr std::string_view kNoProtocolMsg = R"(This is not an HTTP message)";

// TODO(yzhao): We'd better rewrite the test to use BCCWrapper directly, instead of
// SocketTraceConnector, to avoid triggering the userland parsing code, so these tests do not need
// change if we alter output format.

// This test requires docker container with --pid=host so that the container's PID and the
// host machine are identical.

// TODO(yzhao): Apply this pattern to other syscall pairs. An issue is that other syscalls do not
// use scatter buffer. One approach would be to concatenate inner vector to a single string, and
// then feed to the syscall. Another caution is that value-parameterized tests actually discourage
// changing functions being tested according to test parameters. The canonical pattern is using test
// parameters as inputs, but keep the function being tested fixed.
enum class SyscallPair {
  kSendRecv,
  kWriteRead,
  kSendMsgRecvMsg,
  kWritevReadv,
};

struct SocketTraceBPFTestParams {
  SyscallPair syscall_pair;
  uint64_t trace_role;
};

class SocketTraceBPFTest : public testing::SocketTraceBPFTest</* TClientSideTracing */ true> {
 protected:
  StatusOr<const ConnTracker*> GetConnTracker(int pid, int fd) {
    PL_ASSIGN_OR_RETURN(const ConnTracker* tracker, source_->GetConnTracker(pid, fd));
    if (tracker == nullptr) {
      return error::Internal("No ConnTracker found for pid=$0 fd=$1", pid, fd);
    }
    return tracker;
  }
};

class NonVecSyscallTests : public SocketTraceBPFTest,
                           public ::testing::WithParamInterface<SocketTraceBPFTestParams> {};

TEST_P(NonVecSyscallTests, NonVecSyscalls) {
  SocketTraceBPFTestParams p = GetParam();
  LOG(INFO) << absl::Substitute("Test parameters: syscall_pair=$0 trace_role=$1",
                                magic_enum::enum_name(p.syscall_pair), p.trace_role);
  ConfigureBPFCapture(kProtocolHTTP, p.trace_role);

  StartTransferDataThread(kHTTPTableNum, kHTTPTable);

  testing::SendRecvScript script({
      {{kHTTPReqMsg1}, {kHTTPRespMsg1}},
      {{kHTTPReqMsg2}, {kHTTPRespMsg2}},
  });

  testing::ClientServerSystem system;
  switch (p.syscall_pair) {
    case SyscallPair::kWriteRead:
      system.RunClientServer<&TCPSocket::Read, &TCPSocket::Write>(script);
      break;
    case SyscallPair::kSendRecv:
      system.RunClientServer<&TCPSocket::Recv, &TCPSocket::Send>(script);
      break;
    default:
      LOG(FATAL) << absl::Substitute("$0 not supported by this test",
                                     magic_enum::enum_name(p.syscall_pair));
  }

  std::vector<TaggedRecordBatch> tablets = StopTransferDataThread();
  ASSERT_FALSE(tablets.empty());

  if (p.trace_role & kRoleClient) {
    ColumnWrapperRecordBatch records =
        FindRecordsMatchingPID(tablets[0].records, kHTTPUPIDIdx, system.ClientPID());

    ASSERT_THAT(records, Each(ColWrapperSizeIs(2)));

    EXPECT_THAT(records[kHTTPRespHeadersIdx]->Get<types::StringValue>(0), HasSubstr("msg1"));
    EXPECT_THAT(records[kHTTPRespHeadersIdx]->Get<types::StringValue>(1), HasSubstr("msg2"));

    // Additional verifications. These are common to all HTTP1.x tracing, so we decide to not
    // duplicate them on all relevant tests.
    EXPECT_EQ(1, records[kHTTPMajorVersionIdx]->Get<types::Int64Value>(0).val);
    EXPECT_EQ(static_cast<uint64_t>(HTTPContentType::kJSON),
              records[kHTTPContentTypeIdx]->Get<types::Int64Value>(0).val);
    EXPECT_EQ(1, records[kHTTPMajorVersionIdx]->Get<types::Int64Value>(1).val);
    EXPECT_EQ(static_cast<uint64_t>(HTTPContentType::kJSON),
              records[kHTTPContentTypeIdx]->Get<types::Int64Value>(1).val);
  }

  if (p.trace_role & kRoleServer) {
    ColumnWrapperRecordBatch records =
        FindRecordsMatchingPID(tablets[0].records, kHTTPUPIDIdx, system.ServerPID());

    ASSERT_THAT(records, Each(ColWrapperSizeIs(2)));

    EXPECT_THAT(records[kHTTPRespHeadersIdx]->Get<types::StringValue>(0), HasSubstr("msg1"));
    EXPECT_THAT(records[kHTTPRespHeadersIdx]->Get<types::StringValue>(1), HasSubstr("msg2"));

    // Additional verifications. These are common to all HTTP1.x tracing, so we decide to not
    // duplicate them on all relevant tests.
    EXPECT_EQ(1, records[kHTTPMajorVersionIdx]->Get<types::Int64Value>(0).val);
    EXPECT_EQ(static_cast<uint64_t>(HTTPContentType::kJSON),
              records[kHTTPContentTypeIdx]->Get<types::Int64Value>(0).val);
    EXPECT_EQ(1, records[kHTTPMajorVersionIdx]->Get<types::Int64Value>(1).val);
    EXPECT_EQ(static_cast<uint64_t>(HTTPContentType::kJSON),
              records[kHTTPContentTypeIdx]->Get<types::Int64Value>(1).val);
  }
}

INSTANTIATE_TEST_SUITE_P(
    NonVecSyscalls, NonVecSyscallTests,
    ::testing::Values(SocketTraceBPFTestParams{SyscallPair::kWriteRead, kRoleClient},
                      SocketTraceBPFTestParams{SyscallPair::kWriteRead, kRoleServer},
                      SocketTraceBPFTestParams{SyscallPair::kWriteRead, kRoleClient | kRoleServer},
                      SocketTraceBPFTestParams{SyscallPair::kSendRecv, kRoleClient},
                      SocketTraceBPFTestParams{SyscallPair::kSendRecv, kRoleServer},
                      SocketTraceBPFTestParams{SyscallPair::kSendRecv, kRoleClient | kRoleServer}));

class IOVecSyscallTests : public SocketTraceBPFTest,
                          public ::testing::WithParamInterface<SocketTraceBPFTestParams> {};

TEST_P(IOVecSyscallTests, IOVecSyscalls) {
  SocketTraceBPFTestParams p = GetParam();
  LOG(INFO) << absl::Substitute("$0 $1", magic_enum::enum_name(p.syscall_pair), p.trace_role);
  ConfigureBPFCapture(kProtocolHTTP, p.trace_role);

  StartTransferDataThread(kHTTPTableNum, kHTTPTable);

  testing::SendRecvScript script({
      {{kHTTPReqMsg1},
       {"HTTP/1.1 200 OK\r\n", "Content-Type: json\r\n", "Content-Length: 1\r\n\r\na"}},
      {{kHTTPReqMsg2},
       {"HTTP/1.1 404 Not Found\r\n", "Content-Type: json\r\n", "Content-Length: 2\r\n\r\nbc"}},
  });

  testing::ClientServerSystem system;
  switch (p.syscall_pair) {
    case SyscallPair::kSendMsgRecvMsg:
      system.RunClientServer<&TCPSocket::RecvMsg, &TCPSocket::SendMsg>(script);
      break;
    case SyscallPair::kWritevReadv:
      system.RunClientServer<&TCPSocket::ReadV, &TCPSocket::WriteV>(script);
      break;
    default:
      LOG(FATAL) << absl::Substitute("$0 not supported by this test",
                                     magic_enum::enum_name(p.syscall_pair));
  }

  std::vector<TaggedRecordBatch> tablets = StopTransferDataThread();
  ASSERT_FALSE(tablets.empty());

  if (p.trace_role & kRoleServer) {
    ColumnWrapperRecordBatch records =
        FindRecordsMatchingPID(tablets[0].records, kHTTPUPIDIdx, system.ServerPID());

    ASSERT_THAT(records, Each(ColWrapperSizeIs(2)));

    EXPECT_EQ(200, records[kHTTPRespStatusIdx]->Get<types::Int64Value>(0).val);
    EXPECT_THAT(std::string(records[kHTTPRespBodyIdx]->Get<types::StringValue>(0)), StrEq("a"));
    EXPECT_THAT(std::string(records[kHTTPRespMessageIdx]->Get<types::StringValue>(0)), StrEq("OK"));

    EXPECT_EQ(404, records[kHTTPRespStatusIdx]->Get<types::Int64Value>(1).val);
    EXPECT_THAT(std::string(records[kHTTPRespBodyIdx]->Get<types::StringValue>(1)), StrEq("bc"));
    EXPECT_THAT(std::string(records[kHTTPRespMessageIdx]->Get<types::StringValue>(1)),
                StrEq("Not Found"));
  }

  if (p.trace_role & kRoleClient) {
    ColumnWrapperRecordBatch records =
        FindRecordsMatchingPID(tablets[0].records, kHTTPUPIDIdx, system.ClientPID());

    ASSERT_THAT(records, Each(ColWrapperSizeIs(2)));

    EXPECT_EQ(200, records[kHTTPRespStatusIdx]->Get<types::Int64Value>(0).val);
    EXPECT_THAT(std::string(records[kHTTPRespBodyIdx]->Get<types::StringValue>(0)), StrEq("a"));
    EXPECT_THAT(std::string(records[kHTTPRespMessageIdx]->Get<types::StringValue>(0)), StrEq("OK"));

    EXPECT_EQ(404, records[kHTTPRespStatusIdx]->Get<types::Int64Value>(1).val);
    EXPECT_THAT(std::string(records[kHTTPRespBodyIdx]->Get<types::StringValue>(1)), StrEq("bc"));
    EXPECT_THAT(std::string(records[kHTTPRespMessageIdx]->Get<types::StringValue>(1)),
                StrEq("Not Found"));
  }
}

INSTANTIATE_TEST_SUITE_P(IOVecSyscalls, IOVecSyscallTests,
                         ::testing::Values(SocketTraceBPFTestParams{SyscallPair::kSendMsgRecvMsg,
                                                                    kRoleClient | kRoleServer},
                                           SocketTraceBPFTestParams{SyscallPair::kWritevReadv,
                                                                    kRoleClient | kRoleServer}));

// Tests that SocketTraceConnector won't send data from BPF to userspace if the data were not
// any of the supported protocols.
TEST_F(SocketTraceBPFTest, NoProtocolWritesNotCaptured) {
  testing::SendRecvScript script({
      {{kNoProtocolMsg}, {kNoProtocolMsg}},
      {{kNoProtocolMsg}, {kNoProtocolMsg}},
  });

  testing::ClientServerSystem system;
  system.RunClientServer<&TCPSocket::Read, &TCPSocket::Write>(script);

  source_->PollPerfBuffers();

  // We would still see the ConnTracker for client and server processes because the connect()
  // accept() calls were traced.

  ASSERT_OK_AND_ASSIGN(const auto* tracker, GetConnTracker(system.ClientPID(), system.ClientFD()));
  EXPECT_TRUE(tracker->send_data().data_buffer().empty());
  EXPECT_TRUE(tracker->recv_data().data_buffer().empty());

  ASSERT_OK_AND_ASSIGN(tracker, GetConnTracker(system.ServerPID(), system.ServerFD()));
  EXPECT_TRUE(tracker->send_data().data_buffer().empty());
  EXPECT_TRUE(tracker->recv_data().data_buffer().empty());
}

TEST_F(SocketTraceBPFTest, MultipleConnections) {
  ConfigureBPFCapture(TrafficProtocol::kProtocolHTTP, kRoleClient);

  StartTransferDataThread(kHTTPTableNum, kHTTPTable);

  // Two separate connections.

  testing::SendRecvScript script1({
      {{kHTTPReqMsg1}, {kHTTPRespMsg1}},
  });
  testing::ClientServerSystem system1;
  system1.RunClientServer<&TCPSocket::Read, &TCPSocket::Write>(script1);

  testing::SendRecvScript script2({
      {{kHTTPReqMsg2}, {kHTTPRespMsg2}},
  });
  testing::ClientServerSystem system2;
  system2.RunClientServer<&TCPSocket::Read, &TCPSocket::Write>(script2);

  std::vector<TaggedRecordBatch> tablets = StopTransferDataThread();
  ASSERT_FALSE(tablets.empty());

  {
    ColumnWrapperRecordBatch records =
        FindRecordsMatchingPID(tablets[0].records, kHTTPUPIDIdx, system1.ClientPID());

    ASSERT_THAT(records, Each(ColWrapperSizeIs(1)));
    EXPECT_THAT(records[kHTTPRespHeadersIdx]->Get<types::StringValue>(0), HasSubstr("msg1"));
  }

  {
    ColumnWrapperRecordBatch records =
        FindRecordsMatchingPID(tablets[0].records, kHTTPUPIDIdx, system2.ClientPID());

    ASSERT_THAT(records, Each(ColWrapperSizeIs(1)));
    EXPECT_THAT(records[kHTTPRespHeadersIdx]->Get<types::StringValue>(0), HasSubstr("msg2"));
  }
}

// Tests that the start time of UPIDs reported in data table are within a specified time window.
TEST_F(SocketTraceBPFTest, StartTime) {
  ConfigureBPFCapture(TrafficProtocol::kProtocolHTTP, kRoleClient);

  StartTransferDataThread(kHTTPTableNum, kHTTPTable);

  testing::SendRecvScript script({
      {{kHTTPReqMsg1}, {kHTTPRespMsg1}},
      {{kHTTPReqMsg2}, {kHTTPRespMsg2}},
  });

  testing::ClientServerSystem system;
  system.RunClientServer<&TCPSocket::Recv, &TCPSocket::Send>(script);

  // Kernel uses a special monotonic clock as start_time, so we must do the same.
  auto now = px::chrono::boot_clock::now();

  // Use a time window to make sure the recorded PID start_time is right.
  // Being super generous with the window, just in case test runs slow.
  auto time_window_start_tp = now - std::chrono::minutes(30);
  auto time_window_end_tp = now + std::chrono::minutes(5);

  // Start times are reported by Linux in what is essentially 10 ms units.
  constexpr int64_t kNsecondsPerSecond = 1000 * 1000 * 1000;
  constexpr int64_t kClockTicks = 100;
  constexpr int64_t kDivFactor = kNsecondsPerSecond / kClockTicks;

  auto time_window_start = time_window_start_tp.time_since_epoch().count() / kDivFactor;
  auto time_window_end = time_window_end_tp.time_since_epoch().count() / kDivFactor;

  std::vector<TaggedRecordBatch> tablets = StopTransferDataThread();
  ASSERT_FALSE(tablets.empty());
  ColumnWrapperRecordBatch records =
      FindRecordsMatchingPID(tablets[0].records, kHTTPUPIDIdx, system.ClientPID());

  ASSERT_THAT(records, Each(ColWrapperSizeIs(2)));

  md::UPID upid0(records[kHTTPUPIDIdx]->Get<types::UInt128Value>(0).val);
  EXPECT_EQ(system.ClientPID(), upid0.pid());
  EXPECT_LT(time_window_start, upid0.start_ts());
  EXPECT_GT(time_window_end, upid0.start_ts());

  md::UPID upid1(records[kHTTPUPIDIdx]->Get<types::UInt128Value>(1).val);
  EXPECT_EQ(system.ClientPID(), upid1.pid());
  EXPECT_LT(time_window_start, upid1.start_ts());
  EXPECT_GT(time_window_end, upid1.start_ts());
}

// Run a UDP-based client-server system.
class UDPSocketTraceBPFTest : public SocketTraceBPFTest {
 protected:
  void SetUp() override {
    SocketTraceBPFTest::SetUp();
    ConfigureBPFCapture(TrafficProtocol::kProtocolHTTP, kRoleClient | kRoleServer);
    server_.BindAndListen();

    pid_ = getpid();
    LOG(INFO) << absl::Substitute("PID=$0", pid_);

    // Drain the perf buffers before beginning the test to make sure perf buffers are empty.
    // Otherwise, the test may flake due to events not being received in user-space.
    source_->PollPerfBuffers();

    // Uncomment to enable tracing:
    // FLAGS_stirling_conn_trace_pid = pid_;
  }

  UDPSocket client_;
  UDPSocket server_;
  int pid_ = 0;
};

TEST_F(UDPSocketTraceBPFTest, UDPSendToRecvFrom) {
  std::string recv_data;

  ASSERT_EQ(client_.SendTo(kHTTPReqMsg1, server_.sockaddr()), kHTTPReqMsg1.size());
  struct sockaddr_in server_remote = server_.RecvFrom(&recv_data);
  ASSERT_NE(server_remote.sin_addr.s_addr, 0);
  ASSERT_NE(server_remote.sin_port, 0);
  EXPECT_EQ(recv_data, kHTTPReqMsg1);

  ASSERT_EQ(server_.SendTo(kHTTPRespMsg1, server_remote), kHTTPRespMsg1.size());
  struct sockaddr_in client_remote = client_.RecvFrom(&recv_data);
  ASSERT_EQ(client_remote.sin_addr.s_addr, server_.addr().s_addr);
  ASSERT_EQ(client_remote.sin_port, server_.port());
  EXPECT_EQ(recv_data, kHTTPRespMsg1);

  source_->PollPerfBuffers();

  ASSERT_OK_AND_ASSIGN(const auto* tracker, GetConnTracker(pid_, client_.sockfd()));
  EXPECT_EQ(tracker->send_data().data_buffer().Head(), kHTTPReqMsg1);
  EXPECT_EQ(tracker->recv_data().data_buffer().Head(), kHTTPRespMsg1);

  ASSERT_OK_AND_ASSIGN(tracker, GetConnTracker(pid_, server_.sockfd()));
  EXPECT_EQ(tracker->send_data().data_buffer().Head(), kHTTPRespMsg1);
  EXPECT_EQ(tracker->recv_data().data_buffer().Head(), kHTTPReqMsg1);
}

TEST_F(UDPSocketTraceBPFTest, UDPSendMsgRecvMsg) {
  std::string recv_data;

  ASSERT_EQ(client_.SendMsg(kHTTPReqMsg1, server_.sockaddr()), kHTTPReqMsg1.size());
  struct sockaddr_in client_sockaddr = server_.RecvMsg(&recv_data);
  ASSERT_NE(client_sockaddr.sin_addr.s_addr, 0);
  ASSERT_NE(client_sockaddr.sin_port, 0);
  EXPECT_EQ(recv_data, kHTTPReqMsg1);

  ASSERT_EQ(server_.SendMsg(kHTTPRespMsg1, client_sockaddr), kHTTPRespMsg1.size());
  struct sockaddr_in client_remote = client_.RecvMsg(&recv_data);
  ASSERT_EQ(client_remote.sin_addr.s_addr, server_.addr().s_addr);
  ASSERT_EQ(client_remote.sin_port, server_.port());
  EXPECT_EQ(recv_data, kHTTPRespMsg1);

  source_->PollPerfBuffers();

  ASSERT_OK_AND_ASSIGN(const auto* tracker, GetConnTracker(pid_, client_.sockfd()));
  EXPECT_EQ(tracker->send_data().data_buffer().Head(), kHTTPReqMsg1);
  EXPECT_EQ(tracker->recv_data().data_buffer().Head(), kHTTPRespMsg1);

  ASSERT_OK_AND_ASSIGN(tracker, GetConnTracker(pid_, server_.sockfd()));
  EXPECT_EQ(tracker->send_data().data_buffer().Head(), kHTTPRespMsg1);
  EXPECT_EQ(tracker->recv_data().data_buffer().Head(), kHTTPReqMsg1);
}

TEST_F(UDPSocketTraceBPFTest, UDPSendMMsgRecvMMsg) {
  std::string recv_data;

  ASSERT_EQ(client_.SendMMsg(kHTTPReqMsg1, server_.sockaddr()), kHTTPReqMsg1.size());
  struct sockaddr_in server_remote = server_.RecvMMsg(&recv_data);
  ASSERT_NE(server_remote.sin_addr.s_addr, 0);
  ASSERT_NE(server_remote.sin_port, 0);
  EXPECT_EQ(recv_data, kHTTPReqMsg1);

  ASSERT_EQ(server_.SendMMsg(kHTTPRespMsg1, server_remote), kHTTPRespMsg1.size());
  struct sockaddr_in client_remote = client_.RecvMMsg(&recv_data);
  ASSERT_EQ(client_remote.sin_addr.s_addr, server_.addr().s_addr);
  ASSERT_EQ(client_remote.sin_port, server_.port());
  EXPECT_EQ(recv_data, kHTTPRespMsg1);

  source_->PollPerfBuffers();

  ASSERT_OK_AND_ASSIGN(const auto* tracker, GetConnTracker(pid_, client_.sockfd()));
  EXPECT_EQ(tracker->send_data().data_buffer().Head(), kHTTPReqMsg1);
  EXPECT_EQ(tracker->recv_data().data_buffer().Head(), kHTTPRespMsg1);

  ASSERT_OK_AND_ASSIGN(tracker, GetConnTracker(pid_, server_.sockfd()));
  EXPECT_EQ(tracker->send_data().data_buffer().Head(), kHTTPRespMsg1);
  EXPECT_EQ(tracker->recv_data().data_buffer().Head(), kHTTPReqMsg1);
}

// A failed non-blocking receive call shouldn't interfere with tracing.
TEST_F(UDPSocketTraceBPFTest, NonBlockingRecv) {
  std::string recv_data;

  // This receive will fail with with EAGAIN, since there's no data to receive.
  struct sockaddr_in failed_recv_remote = client_.RecvFrom(&recv_data, MSG_DONTWAIT);
  ASSERT_EQ(failed_recv_remote.sin_addr.s_addr, 0);
  ASSERT_EQ(failed_recv_remote.sin_port, 0);
  ASSERT_TRUE(recv_data.empty());

  ASSERT_EQ(client_.SendTo(kHTTPReqMsg1, server_.sockaddr()), kHTTPReqMsg1.size());
  struct sockaddr_in server_remote = server_.RecvFrom(&recv_data);
  ASSERT_NE(server_remote.sin_addr.s_addr, 0);
  ASSERT_NE(server_remote.sin_port, 0);
  EXPECT_EQ(recv_data, kHTTPReqMsg1);

  ASSERT_EQ(server_.SendTo(kHTTPRespMsg1, server_remote), kHTTPRespMsg1.size());
  struct sockaddr_in client_remote = client_.RecvFrom(&recv_data);
  ASSERT_EQ(client_remote.sin_addr.s_addr, server_.addr().s_addr);
  ASSERT_EQ(client_remote.sin_port, server_.port());
  EXPECT_EQ(recv_data, kHTTPRespMsg1);

  source_->PollPerfBuffers();

  ASSERT_OK_AND_ASSIGN(const auto* tracker, GetConnTracker(pid_, client_.sockfd()));
  EXPECT_EQ(tracker->send_data().data_buffer().Head(), kHTTPReqMsg1);
  EXPECT_EQ(tracker->recv_data().data_buffer().Head(), kHTTPRespMsg1);
  EXPECT_EQ(tracker->remote_endpoint().port(), ntohs(server_.sockaddr().sin_port));
  EXPECT_EQ(tracker->remote_endpoint().AddrStr(), "127.0.0.1");

  ASSERT_OK_AND_ASSIGN(tracker, GetConnTracker(pid_, server_.sockfd()));
  EXPECT_EQ(tracker->send_data().data_buffer().Head(), kHTTPRespMsg1);
  EXPECT_EQ(tracker->recv_data().data_buffer().Head(), kHTTPReqMsg1);
  EXPECT_EQ(tracker->remote_endpoint().port(), ntohs(server_remote.sin_port));
  EXPECT_EQ(tracker->remote_endpoint().AddrStr(), "127.0.0.1");
}

class SocketTraceServerSideBPFTest
    : public testing::SocketTraceBPFTest</* TClientSideTracing */ false> {};

uint64_t GetConnStats(const ConnTracker& tracker, ConnTracker::Stats::Key key) {
  return tracker.stats().Get(key);
}

uint64_t GetBytesSentTransferred(const ConnTracker& tracker) {
  return GetConnStats(tracker, ConnTracker::Stats::Key::kBytesSentTransferred);
}

uint64_t GetBytesRecvTransferred(const ConnTracker& tracker) {
  return GetConnStats(tracker, ConnTracker::Stats::Key::kBytesRecvTransferred);
}

uint64_t GetBytesSent(const ConnTracker& tracker) {
  return GetConnStats(tracker, ConnTracker::Stats::Key::kBytesSent);
}

uint64_t GetBytesRecv(const ConnTracker& tracker) {
  return GetConnStats(tracker, ConnTracker::Stats::Key::kBytesRecv);
}

uint64_t GetDataEventSent(const ConnTracker& tracker) {
  return GetConnStats(tracker, ConnTracker::Stats::Key::kDataEventSent);
}

uint64_t GetDataEventRecv(const ConnTracker& tracker) {
  return GetConnStats(tracker, ConnTracker::Stats::Key::kDataEventRecv);
}

uint64_t GetValidRecords(const ConnTracker& tracker) {
  return GetConnStats(tracker, ConnTracker::Stats::Key::kValidRecords);
}

uint64_t GetInvalidRecords(const ConnTracker& tracker) {
  return GetConnStats(tracker, ConnTracker::Stats::Key::kInvalidRecords);
}

// Tests that connection stats are updated after ConnTracker is disabled because of being
// a client.
TEST_F(SocketTraceServerSideBPFTest, ConnStatsUpdatedAfterConnTrackerDisabled) {
  auto* socket_trace_connector = dynamic_cast<SocketTraceConnector*>(source_.get());

  ConfigureBPFCapture(kProtocolHTTP, kRoleClient | kRoleServer);
  DataTable data_table(kHTTPTable);

  // Drain the perf buffers before stimulus activity.
  // Otherwise, perf buffers may fill up, causing lost events and flaky test results.
  source_->PollPerfBuffers();

  TCPSocket client;
  TCPSocket server;

  server.BindAndListen();
  client.Connect(server);
  auto server_endpoint = server.Accept();

  // First write.
  // BPF should trace the write due to `ConfigureBPFCapture(kProtocolHTTP, kRoleClient |
  // kRoleServer)` above. Then TransferData should disable the client-side tracing in user-space,
  // and propagate the information back to BPF.
  ASSERT_TRUE(client.Write(kHTTPReqMsg1));
  std::string msg;
  ASSERT_TRUE(server_endpoint->Recv(&msg));

  ASSERT_TRUE(server_endpoint->Send(kHTTPRespMsg1));
  ASSERT_TRUE(client.Recv(&msg));

  sleep(1);
  source_->TransferData(ctx_.get(), kHTTPTableNum, &data_table);

  ASSERT_OK_AND_ASSIGN(const ConnTracker* client_side_tracker,
                       socket_trace_connector->GetConnTracker(getpid(), client.sockfd()));
  ASSERT_OK_AND_ASSIGN(const ConnTracker* server_side_tracker,
                       socket_trace_connector->GetConnTracker(getpid(), server_endpoint->sockfd()));

  EXPECT_EQ(GetBytesSent(*client_side_tracker), kHTTPReqMsg1.size());
  EXPECT_EQ(GetBytesSentTransferred(*client_side_tracker), kHTTPReqMsg1.size());
  EXPECT_EQ(GetDataEventSent(*client_side_tracker), 1);
  EXPECT_EQ(GetDataEventRecv(*client_side_tracker), 1);
  // No records are produced because client-side tracing is disabled in user-space.
  EXPECT_EQ(GetValidRecords(*client_side_tracker), 0);
  EXPECT_EQ(GetInvalidRecords(*client_side_tracker), 0);

  EXPECT_EQ(GetBytesRecv(*server_side_tracker), kHTTPReqMsg1.size());
  EXPECT_EQ(GetBytesRecvTransferred(*server_side_tracker), kHTTPReqMsg1.size());
  EXPECT_EQ(GetDataEventSent(*server_side_tracker), 1);
  EXPECT_EQ(GetDataEventRecv(*server_side_tracker), 1);
  EXPECT_EQ(GetValidRecords(*server_side_tracker), 1);
  EXPECT_EQ(GetInvalidRecords(*server_side_tracker), 0);

  // Second write.
  // BPF should not even trace the write, because it should have been disabled from user-space.
  ASSERT_TRUE(client.Write(kHTTPReqMsg2));
  ASSERT_TRUE(server_endpoint->Recv(&msg));

  ASSERT_TRUE(server_endpoint->Send(kHTTPRespMsg1));
  ASSERT_TRUE(client.Recv(&msg));
  sleep(1);

  source_->TransferData(ctx_.get(), kHTTPTableNum, &data_table);

  EXPECT_EQ(GetBytesSent(*client_side_tracker), kHTTPReqMsg1.size() + kHTTPReqMsg2.size())
      << "Data sent were increased.";
  EXPECT_EQ(GetBytesSentTransferred(*client_side_tracker), kHTTPReqMsg1.size())
      << "Data were not transferred to user space, so the counter should be the same.";
  EXPECT_EQ(GetDataEventSent(*client_side_tracker), 2);
  EXPECT_EQ(GetDataEventRecv(*client_side_tracker), 2);
  EXPECT_EQ(GetValidRecords(*client_side_tracker), 0);
  EXPECT_EQ(GetInvalidRecords(*client_side_tracker), 0);

  EXPECT_EQ(GetBytesRecv(*server_side_tracker), kHTTPReqMsg1.size() + kHTTPReqMsg2.size());
  EXPECT_EQ(GetBytesRecvTransferred(*server_side_tracker),
            kHTTPReqMsg1.size() + kHTTPReqMsg2.size());
  EXPECT_EQ(GetDataEventSent(*server_side_tracker), 2);
  EXPECT_EQ(GetDataEventRecv(*server_side_tracker), 2);
  EXPECT_EQ(GetValidRecords(*server_side_tracker), 2);
  EXPECT_EQ(GetInvalidRecords(*server_side_tracker), 0);
}

}  // namespace stirling
}  // namespace px

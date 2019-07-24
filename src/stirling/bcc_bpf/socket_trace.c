#include "src/stirling/bcc_bpf/socket_trace.h"

#include <linux/sched.h>
#include <linux/socket.h>
#include <uapi/linux/in6.h>
#include <uapi/linux/ptrace.h>

#include "src/stirling/bcc_bpf/log_event.h"
#include "src/stirling/bcc_bpf/logging.h"

// This keeps instruction count below BPF's limit of 4096 per probe.
// TODO(yzhao): Investigate using tail call to reuse stack space to support loop.
#define LOOP_LIMIT 20

// This is the perf buffer for BPF program to export data from kernel to user space.
BPF_PERF_OUTPUT(socket_http_events);
BPF_PERF_OUTPUT(socket_mysql_events);
BPF_PERF_OUTPUT(socket_open_conns);
BPF_PERF_OUTPUT(socket_close_conns);

/***********************************************************
 * Internal structs and definitions
 ***********************************************************/

struct connect_info_t {
  struct sockaddr_in6 addr;
  int fd;
} __attribute__((__packed__, aligned(8)));

struct accept_info_t {
  struct sockaddr* addr;
  size_t* addrlen;
} __attribute__((__packed__, aligned(8)));

struct data_info_t {
  u32 fd;
  const char* buf;
  // For sendmsg()/recvmsg().
  // TODO(yzhao): We can merge buf as a special case of len==1 with msghdr, so we can use one field
  // instead of two.
  const struct user_msghdr* msghdr;
} __attribute__((__packed__, aligned(8)));

// This control_map is a bit-mask that controls which endpoints are traced in a connection.
// The bits are defined in ReqRespRole enum, kRoleRequestor or kRoleResponder. kRoleUnknown is not
// really used, but is defined for completeness.
// There is a control map element for each protocol.
BPF_PERCPU_ARRAY(control_map, u64, kNumProtocols);

// Map from user-space file descriptors to the connections obtained from accept() syscall.
// Tracks connection from accept() -> close().
// Key is {tgid, fd}.
BPF_HASH(conn_info_map, u64, struct conn_info_t);

// Map from thread to its ongoing accept() syscall's input argument.
// Tracks accept() call from entry -> exit.
// Key is {tgid, pid}.
BPF_HASH(active_accept_info_map, u64, struct accept_info_t);

// Map from thread to its ongoing connect() syscall's input argument.
// Tracks connect() call from entry -> exit.
// Key is {tgid, pid}.
BPF_HASH(active_connect_info_map, u64, struct connect_info_t);

// Map from thread to its ongoing write() syscall's input argument.
// Tracks write() call from entry -> exit.
// Key is {tgid, pid}.
BPF_HASH(active_write_info_map, u64, struct data_info_t);

// Map from thread to its ongoing read() syscall's input argument.
// Tracks read() call from entry -> exit.
// Key is {tgid, pid}.
BPF_HASH(active_read_info_map, u64, struct data_info_t);

// Map from TGID, FD pair to a unique identifier (generation) of that pair.
// Key is {tgid, fd}.
BPF_HASH(proc_conn_map, u64, u32);

static __inline uint64_t get_tgid_start_time() {
  struct task_struct* task = (struct task_struct*)bpf_get_current_task();
  return task->group_leader->start_time;
}

static __inline uint32_t get_tgid_fd_generation(u64 tgid_fd) {
  u32 init_tgid_fd_generation = 0;
  u32* tgid_fd_generation = proc_conn_map.lookup_or_init(&tgid_fd, &init_tgid_fd_generation);
  return (*tgid_fd_generation)++;
}

static __inline bool should_trace(const struct traffic_class_t* traffic_class) {
  u32 protocol = traffic_class->protocol;
  u64 kZero = 0;
  // TODO(yzhao): BCC doc states BPF_PERCPU_ARRAY: all array elements are **pre-allocated with zero
  // values** (https://github.com/iovisor/bcc/blob/master/docs/reference_guide.md). That suggests
  // lookup() suffices. But this seems more robust, as BCC behavior is often not intuitive.
  u64 control = *control_map.lookup_or_init(&protocol, &kZero);
  return control & traffic_class->role;
}

// BPF programs are limited to a 512-byte stack. We store this value per CPU
// and use it as a heap allocated value.
BPF_PERCPU_ARRAY(data_buffer_heap, struct socket_data_event_t, 1);

static __inline struct socket_data_event_t* fill_event(EventType event_type,
                                                       const struct conn_info_t* conn_info) {
  u32 kZero = 0;
  struct socket_data_event_t* event = data_buffer_heap.lookup(&kZero);
  if (event == NULL) {
    return NULL;
  }
  event->attr.event_type = event_type;
  event->attr.timestamp_ns = bpf_ktime_get_ns();
  event->attr.conn_id.tgid = conn_info->conn_id.tgid;
  event->attr.conn_id.tgid_start_time_ns = conn_info->conn_id.tgid_start_time_ns;
  event->attr.conn_id.fd = conn_info->conn_id.fd;
  event->attr.conn_id.generation = conn_info->conn_id.generation;
  event->attr.traffic_class = conn_info->traffic_class;
  return event;
}

/***********************************************************
 * Buffer processing helper functions
 ***********************************************************/

static __inline bool is_http_response(const char* buf, size_t count) {
  // Smallest HTTP response is 17 characters:
  // HTTP/1.1 200 OK\r\n
  // Use 16 here to be conservative.
  if (count < 16) {
    return false;
  }

  if (buf[0] == 'H' && buf[1] == 'T' && buf[2] == 'T' && buf[3] == 'P') {
    return true;
  }

  return false;
}

static __inline bool is_http_request(const char* buf, size_t count) {
  // Smallest HTTP response is 16 characters:
  // GET x HTTP/1.1\r\n
  if (count < 16) {
    return false;
  }

  if (buf[0] == 'G' && buf[1] == 'E' && buf[2] == 'T') {
    return true;
  }
  if (buf[0] == 'P' && buf[1] == 'O' && buf[2] == 'S' && buf[3] == 'T') {
    return true;
  }
  // TODO(oazizi): Should we add PUT, DELETE, HEAD, and perhaps others?

  return false;
}

static __inline bool is_mysql_protocol(const char* buf, size_t count) {
  // Need at least 7 bytes for COM_STMT_PREPARE + SELECT.
  // Plus there needs to be some substance to the query after that.
  // Here, expect at least 8 bytes.
  if (count < 8) {
    return false;
  }

  // MySQL queries start with byte 0x16 (COM_STMT_PREPARE in the MySQL protocol).
  // For now, we also expect the word SELECT to avoid pollution.
  // TODO(oazizi): Make this more robust.
  if (buf[0] == 0x16 && buf[1] == 'S' && buf[2] == 'E' && buf[3] == 'L' && buf[4] == 'E' &&
      buf[5] == 'C' && buf[6] == 'T') {
    return true;
  }

  return false;
}

// Technically, HTTP2 client connection preface is 24 octes [1]. Practically,
// the first 3 shall be sufficient. Note this is sent from client,
// so it would be captured on server's read()/recvfrom()/recvmsg() or client's
// write()/sendto()/sendmsg().
//
// [1] https://http2.github.io/http2-spec/#ConnectionHeader
static __inline bool is_http2_connection_preface(const char* buf, size_t count) {
  if (count < 3) {
    return false;
  }
  return buf[0] == 'P' && buf[1] == 'R' && buf[2] == 'I';
}

typedef enum {
  kEgress,
  kIngress,
} TrafficDirection;

static __inline struct traffic_class_t infer_traffic(TrafficDirection direction, const char* buf,
                                                     size_t count) {
  struct traffic_class_t traffic_class;
  if (is_http_response(buf, count)) {
    traffic_class.protocol = kProtocolHTTP;
    switch (direction) {
      case kEgress:
        traffic_class.role = kRoleResponder;
        break;
      case kIngress:
        traffic_class.role = kRoleRequestor;
        break;
    }
  } else if (is_http_request(buf, count)) {
    traffic_class.protocol = kProtocolHTTP;
    switch (direction) {
      case kEgress:
        traffic_class.role = kRoleRequestor;
        break;
      case kIngress:
        traffic_class.role = kRoleResponder;
        break;
    }
  } else if (is_mysql_protocol(buf, count)) {
    traffic_class.protocol = kProtocolMySQL;
    switch (direction) {
      case kEgress:
        traffic_class.role = kRoleRequestor;
        break;
      case kIngress:
        traffic_class.role = kRoleResponder;
        break;
    }
  } else if (is_http2_connection_preface(buf, count)) {
    traffic_class.protocol = kProtocolHTTP2;
    switch (direction) {
      case kEgress:
        traffic_class.role = kRoleRequestor;
        break;
      case kIngress:
        traffic_class.role = kRoleResponder;
        break;
    }
  } else {
    traffic_class.protocol = kProtocolUnknown;
    traffic_class.role = kRoleUnknown;
  }
  return traffic_class;
}

static __inline struct conn_info_t* get_conn_info(u32 tgid, u32 fd) {
  u64 tgid_fd = ((u64)tgid << 32) | (u32)fd;
  struct conn_info_t new_conn_info;
  memset(&new_conn_info, 0, sizeof(struct conn_info_t));
  struct conn_info_t* conn_info = conn_info_map.lookup_or_init(&tgid_fd, &new_conn_info);
  // Use timestamp being zero to detect that a new conn_info was initialized.
  if (conn_info->timestamp_ns == 0) {
    // If lookup_or_init initialized a new conn_info, we need to set some fields.
    conn_info->conn_id.tgid = tgid;
    conn_info->conn_id.tgid_start_time_ns = get_tgid_start_time();
    conn_info->conn_id.fd = fd;
    conn_info->conn_id.generation = get_tgid_fd_generation(tgid_fd);

    // Unknown accept()/connect(), so no known timestamp either.
    // But have to change timestamp, so set to 1ns.
    conn_info->timestamp_ns = 1;
  }
  return conn_info;
}

// TODO(oazizi): This function should go away once the protocol is identified externally.
//               Also, could move this function into the header file, so we can test it.
static __inline void update_traffic_class(struct conn_info_t* conn_info, TrafficDirection direction,
                                          const char* buf, size_t count) {
  // TODO(oazizi): Future architecture should have user-land provide the traffic_class.
  // TODO(oazizi): conn_info currently works only if tracing on the send or recv side of a process,
  //               but not both simultaneously, because we need to mark two traffic classes.

  // Try to infer connection type (protocol) based on data.
  // If protocol is detected, then let it through, even though accept()/connect() was not captured.
  if (conn_info != NULL && conn_info->traffic_class.protocol == kProtocolUnknown) {
    // TODO(oazizi): Look for only certain protocols on write/send()?
    struct traffic_class_t traffic_class = infer_traffic(direction, buf, count);
    conn_info->traffic_class = traffic_class;
  }
}

// This specify one pid to monitor. This is used during test to eliminate noise.
// TODO(yzhao): We need a more robust mechanism for production use, which should be able to:
// * Specify multiple pids up to a certain limit, let's say 1024.
// * Support efficient lookup inside bpf to minimize overhead.
BPF_PERCPU_ARRAY(test_only_target_tgid, s64, 1);
static __inline bool test_only_should_trace_tgid(const u32 tgid) {
  int kZero = 0;
  s64* target_tgid = test_only_target_tgid.lookup(&kZero);
  if (target_tgid == NULL) {
    return true;
  }
  if (*target_tgid < 0) {
    return true;
  }
  return *target_tgid == tgid;
}

/***********************************************************
 * BPF syscall probe functions
 ***********************************************************/

static __inline void submit_new_conn(struct pt_regs* ctx, u32 tgid, u32 fd,
                                     struct sockaddr_in6 addr) {
  u64 tgid_fd = ((u64)tgid << 32) | (u32)fd;

  struct conn_info_t conn_info;
  memset(&conn_info, 0, sizeof(struct conn_info_t));
  conn_info.timestamp_ns = bpf_ktime_get_ns();
  conn_info.addr = addr;
  conn_info.traffic_class.protocol = kProtocolUnknown;
  conn_info.traffic_class.role = kRoleUnknown;
  conn_info.wr_seq_num = 0;
  conn_info.rd_seq_num = 0;
  conn_info.conn_id.tgid = tgid;
  conn_info.conn_id.tgid_start_time_ns = get_tgid_start_time();
  conn_info.conn_id.fd = fd;
  conn_info.conn_id.generation = get_tgid_fd_generation(tgid_fd);

  conn_info_map.update(&tgid_fd, &conn_info);
  socket_open_conns.perf_submit(ctx, &conn_info, sizeof(struct conn_info_t));
}

static __inline int probe_entry_connect_impl(struct pt_regs* ctx, int sockfd,
                                             const struct sockaddr* addr, size_t addrlen) {
  u64 id = bpf_get_current_pid_tgid();

  // Only record IP (IPV4 and IPV6) connections.
  if (!(addr->sa_family == AF_INET || addr->sa_family == AF_INET6)) {
    return 0;
  }

  struct connect_info_t connect_info;

  bpf_probe_read(&connect_info.addr, sizeof(struct sockaddr_in6), (const void*)addr);
  connect_info.fd = sockfd;

  active_connect_info_map.update(&id, &connect_info);

  return 0;
}

static __inline int probe_ret_connect_impl(struct pt_regs* ctx) {
  u64 id = bpf_get_current_pid_tgid();
  u32 tgid = id >> 32;

  int ret_val = PT_REGS_RC(ctx);
  if (ret_val < 0) {
    goto done;
  }

  struct connect_info_t* connect_info = active_connect_info_map.lookup(&id);
  if (connect_info == NULL) {
    goto done;
  }

  submit_new_conn(ctx, tgid, (u32)connect_info->fd, connect_info->addr);

done:
  // Regardless of what happened, after accept() returns, there is no need to track the sock_addr
  // being accepted during the accept() call, therefore we can remove the entry.
  active_connect_info_map.delete(&id);
  return 0;
}

// This function stores the address to the sockaddr struct in the active_accept_info_map map.
// The key is the current pid/tgid.
//
// TODO(yzhao): We are not able to trace the source address/port yet. We might need to probe the
// socket() syscall.
static __inline int probe_entry_accept_impl(struct pt_regs* ctx, int sockfd, struct sockaddr* addr,
                                            size_t* addrlen) {
  u64 id = bpf_get_current_pid_tgid();
  u32 tgid = id >> 32;

  if (!test_only_should_trace_tgid(tgid)) {
    return 0;
  }

  struct accept_info_t accept_info;
  accept_info.addr = addr;
  accept_info.addrlen = addrlen;
  active_accept_info_map.update(&id, &accept_info);

  return 0;
}

// Read the sockaddr values and write to the output buffer.
static __inline int probe_ret_accept_impl(struct pt_regs* ctx) {
  u64 id = bpf_get_current_pid_tgid();
  u32 tgid = id >> 32;

  int ret_fd = PT_REGS_RC(ctx);
  if (ret_fd < 0) {
    goto done;
  }

  struct accept_info_t* accept_info = active_accept_info_map.lookup(&id);
  if (accept_info == NULL) {
    goto done;
  }

  // Only record IP (IPV4 and IPV6) connections.
  if (!(accept_info->addr->sa_family == AF_INET || accept_info->addr->sa_family == AF_INET6)) {
    goto done;
  }

  submit_new_conn(ctx, tgid, (u32)ret_fd, *((struct sockaddr_in6*)accept_info->addr));
done:
  // Regardless of what happened, after accept() returns, there is no need to track the sock_addr
  // being accepted during the accept() call, therefore we can remove the entry.
  active_accept_info_map.delete(&id);
  return 0;
}

static __inline void update_traffic_class_for_msghdr(struct conn_info_t* conn_info,
                                                     TrafficDirection direction,
                                                     const struct user_msghdr* msghdr) {
  size_t msg_iovlen = 0;
  bpf_probe_read(&msg_iovlen, sizeof(msg_iovlen), &msghdr->msg_iovlen);
  if (msg_iovlen == 0) {
    return;
  }
  const struct iovec* msg_iov = NULL;
  bpf_probe_read(&msg_iov, sizeof(void*), &msghdr->msg_iov);
  if (msg_iov == NULL) {
    return;
  }
  struct iovec iov;
  bpf_probe_read(&iov, sizeof(struct iovec), &msg_iov[0]);
  update_traffic_class(conn_info, direction, iov.iov_base, iov.iov_len);
}

static __inline int probe_entry_write_send(struct pt_regs* ctx, int fd, char* buf, size_t count,
                                           const struct user_msghdr* msghdr) {
  if (fd < 0) {
    return 0;
  }

  u64 id = bpf_get_current_pid_tgid();
  u32 tgid = id >> 32;

  if (!test_only_should_trace_tgid(tgid)) {
    return 0;
  }

  struct conn_info_t* conn_info = get_conn_info(tgid, fd);
  if (conn_info == NULL) {
    return 0;
  }

  // TODO(yzhao): Split the interface such that the singular buf case and multiple bufs in msghdr
  // are handled separately without mixed interface. The plan is to factor out helper functions for
  // lower-level functionalities, and call them separately for each case.
  if (buf != NULL) {
    update_traffic_class(conn_info, kEgress, buf, count);
  } else if (msghdr != NULL) {
    update_traffic_class_for_msghdr(conn_info, kEgress, msghdr);
  }

  // If this connection has an unknown protocol, abort (to avoid pollution).
  // TODO(oazizi/yzhao): We can remove this after we have the ability to identify connections of
  // interests, through tgid + fd.
  if (conn_info->traffic_class.protocol == kProtocolUnknown) {
    return 0;
  }

  // Filter for request or response based on control flags and protocol type.
  if (!should_trace(&conn_info->traffic_class)) {
    return 0;
  }

  struct data_info_t write_info;
  memset(&write_info, 0, sizeof(struct data_info_t));
  write_info.fd = fd;
  write_info.buf = buf;
  write_info.msghdr = msghdr;

  active_write_info_map.update(&id, &write_info);
  return 0;
}

// TODO(yzhao): We can write a test for this, by define a dummy bpf_probe_read() function. Similar
// in idea to a mock in normal C++ code.
//
// Writes the input buf to event, and submits the event to the corresponding perf buffer.
//
// Returns the bytes output from the input buf. Note that is not the total bytes submitted to the
// perf buffer, which includes additional metadata.
static __inline size_t perf_submit_buf(struct pt_regs* ctx, const TrafficDirection direction,
                                       const char* buf, const size_t buf_size,
                                       struct conn_info_t* conn_info,
                                       struct socket_data_event_t* event) {
  switch (direction) {
    case kEgress:
      event->attr.seq_num = conn_info->wr_seq_num;
      ++conn_info->wr_seq_num;
      break;
    case kIngress:
      event->attr.seq_num = conn_info->rd_seq_num;
      ++conn_info->rd_seq_num;
      break;
  }

  const size_t msg_size = buf_size < sizeof(event->msg) ? buf_size : sizeof(event->msg);
  event->attr.msg_size = msg_size;
  bpf_probe_read(&event->msg, msg_size, buf);

  // Write snooped arguments to perf ring buffer.
  const size_t size_to_submit = sizeof(event->attr) + msg_size;
  switch (conn_info->traffic_class.protocol) {
    case kProtocolHTTP:
    case kProtocolHTTP2:
      socket_http_events.perf_submit(ctx, event, size_to_submit);
      break;
    case kProtocolMySQL:
      socket_mysql_events.perf_submit(ctx, event, size_to_submit);
      break;
    default:
      break;
  }
  return msg_size;
}

static __inline void perf_submit_msghdr(struct pt_regs* ctx, const TrafficDirection direction,
                                        const struct user_msghdr* msghdr, const ssize_t total_size,
                                        struct conn_info_t* conn_info,
                                        struct socket_data_event_t* event) {
  size_t msg_iovlen = 0;
  bpf_probe_read(&msg_iovlen, sizeof(size_t), &msghdr->msg_iovlen);

  const struct iovec* iov_src = NULL;
  bpf_probe_read(&iov_src, sizeof(void*), &msghdr->msg_iov);

#pragma unroll
  for (int i = 0, bytes_copied = 0; i < LOOP_LIMIT && i < msg_iovlen && bytes_copied < total_size;
       ++i) {
    struct iovec iov;
    bpf_probe_read(&iov, sizeof(struct iovec), &iov_src[i]);
    const size_t bytes_remain = total_size - bytes_copied;
    const size_t iov_len = iov.iov_len < bytes_remain ? iov.iov_len : bytes_remain;

    const size_t msg_size =
        perf_submit_buf(ctx, direction, iov.iov_base, iov_len, conn_info, event);
    bytes_copied += msg_size;
  }
}

static __inline int probe_ret_write_send(struct pt_regs* ctx, EventType event_type) {
  u64 id = bpf_get_current_pid_tgid();
  u32 tgid = id >> 32;

  ssize_t bytes_written = PT_REGS_RC(ctx);
  if (bytes_written <= 0) {
    // This write() call failed, or has nothing to write.
    goto done;
  }

  const struct data_info_t* write_info = active_write_info_map.lookup(&id);
  if (write_info == NULL) {
    goto done;
  }

  u64 tgid_fd = ((u64)tgid << 32) | (u32)write_info->fd;
  struct conn_info_t* conn_info = conn_info_map.lookup(&tgid_fd);
  if (conn_info == NULL) {
    goto done;
  }

  struct socket_data_event_t* event = fill_event(event_type, conn_info);
  if (event == NULL) {
    goto done;
  }

  // TODO(yzhao): Same TODO for split the interface.
  if (write_info->buf != NULL) {
    perf_submit_buf(ctx, kEgress, write_info->buf, bytes_written, conn_info, event);
  } else if (write_info->msghdr != NULL) {
    perf_submit_msghdr(ctx, kEgress, write_info->msghdr, bytes_written, conn_info, event);
  }

done:
  // Regardless of what happened, after write/send/sendto() returns, there is no need to track the
  // fd & buf being accepted by write/send/sendto() call, therefore we can remove the entry.
  active_write_info_map.delete(&id);
  return 0;
}

static __inline int probe_entry_read_recv(struct pt_regs* ctx, int fd, char* buf, size_t count,
                                          const struct user_msghdr* msghdr) {
  if (fd < 0) {
    return 0;
  }
  u64 id = bpf_get_current_pid_tgid();
  u32 tgid = id >> 32;

  if (!test_only_should_trace_tgid(tgid)) {
    return 0;
  }

  struct data_info_t read_info;
  memset(&read_info, 0, sizeof(struct data_info_t));
  read_info.fd = fd;
  read_info.buf = buf;
  read_info.msghdr = msghdr;

  active_read_info_map.update(&id, &read_info);
  return 0;
}

static __inline int probe_ret_read_recv(struct pt_regs* ctx, EventType event_type) {
  u64 id = bpf_get_current_pid_tgid();
  u32 tgid = id >> 32;

  ssize_t bytes_read = PT_REGS_RC(ctx);

  if (bytes_read <= 0) {
    // This read() call failed, or read nothing.
    goto done;
  }

  const struct data_info_t* read_info = active_read_info_map.lookup(&id);
  if (read_info == NULL) {
    goto done;
  }

  const char* buf = read_info->buf;

  struct conn_info_t* conn_info = get_conn_info(tgid, read_info->fd);
  if (conn_info == NULL) {
    goto done;
  }

  // TODO(yzhao): Same TODO for split the interface.
  if (buf != NULL) {
    update_traffic_class(conn_info, kIngress, buf, bytes_read);
  } else if (read_info->msghdr != NULL) {
    update_traffic_class_for_msghdr(conn_info, kIngress, read_info->msghdr);
  }

  // If this connection has an unknown protocol, abort (to avoid pollution).
  if (conn_info->traffic_class.protocol == kProtocolUnknown) {
    return 0;
  }

  // Filter for request or response based on control flags and protocol type.
  if (!should_trace(&conn_info->traffic_class)) {
    goto done;
  }

  struct socket_data_event_t* event = fill_event(event_type, conn_info);
  if (event == NULL) {
    goto done;
  }

  // TODO(yzhao): Same TODO for split the interface.
  if (read_info->buf != NULL) {
    perf_submit_buf(ctx, kIngress, read_info->buf, bytes_read, conn_info, event);
  } else if (read_info->msghdr != NULL) {
    perf_submit_msghdr(ctx, kIngress, read_info->msghdr, bytes_read, conn_info, event);
  }

done:
  // Regardless of what happened, after write/send() returns, there is no need to track the fd & buf
  // being accepted by write/send() call, therefore we can remove the entry.
  active_read_info_map.delete(&id);
  return 0;
}

static __inline int probe_close_impl(struct pt_regs* ctx, int fd) {
  if (fd < 0) {
    return 0;
  }
  u64 id = bpf_get_current_pid_tgid();
  u32 tgid = id >> 32;

  if (!test_only_should_trace_tgid(tgid)) {
    return 0;
  }

  u64 tgid_fd = ((u64)tgid << 32) | (u32)fd;
  struct conn_info_t* conn_info = conn_info_map.lookup(&tgid_fd);
  if (conn_info == NULL) {
    return 0;
  }
  socket_close_conns.perf_submit(ctx, conn_info, sizeof(struct conn_info_t));
  conn_info_map.delete(&tgid_fd);
  return 0;
}

/***********************************************************
 * BPF syscall probe function entry-points
 ***********************************************************/

int syscall__probe_entry_connect(struct pt_regs* ctx, int sockfd, struct sockaddr* addr,
                                 size_t addrlen) {
  return probe_entry_connect_impl(ctx, sockfd, addr, addrlen);
}

int syscall__probe_ret_connect(struct pt_regs* ctx) { return probe_ret_connect_impl(ctx); }

int syscall__probe_entry_accept(struct pt_regs* ctx, int sockfd, struct sockaddr* addr,
                                size_t* addrlen) {
  return probe_entry_accept_impl(ctx, sockfd, addr, addrlen);
}

int syscall__probe_ret_accept(struct pt_regs* ctx) { return probe_ret_accept_impl(ctx); }

int syscall__probe_entry_accept4(struct pt_regs* ctx, int sockfd, struct sockaddr* addr,
                                 size_t* addrlen) {
  return probe_entry_accept_impl(ctx, sockfd, addr, addrlen);
}

int syscall__probe_ret_accept4(struct pt_regs* ctx) { return probe_ret_accept_impl(ctx); }

int syscall__probe_entry_write(struct pt_regs* ctx, int fd, char* buf, size_t count) {
  return probe_entry_write_send(ctx, fd, buf, count, NULL);
}

int syscall__probe_ret_write(struct pt_regs* ctx) {
  return probe_ret_write_send(ctx, kEventTypeSyscallWriteEvent);
}

int syscall__probe_entry_send(struct pt_regs* ctx, int fd, char* buf, size_t count) {
  return probe_entry_write_send(ctx, fd, buf, count, NULL);
}

int syscall__probe_ret_send(struct pt_regs* ctx) {
  return probe_ret_write_send(ctx, kEventTypeSyscallSendEvent);
}

int syscall__probe_entry_read(struct pt_regs* ctx, int fd, char* buf, size_t count) {
  return probe_entry_read_recv(ctx, fd, buf, count, /*msghdr*/ NULL);
}

int syscall__probe_ret_read(struct pt_regs* ctx) {
  return probe_ret_read_recv(ctx, kEventTypeSyscallReadEvent);
}

int syscall__probe_entry_recv(struct pt_regs* ctx, int fd, char* buf, size_t count) {
  return probe_entry_read_recv(ctx, fd, buf, count, /*msghdr*/ NULL);
}

int syscall__probe_ret_recv(struct pt_regs* ctx) {
  return probe_ret_read_recv(ctx, kEventTypeSyscallRecvEvent);
}

int syscall__probe_entry_sendto(struct pt_regs* ctx, int fd, char* buf, size_t count) {
  return probe_entry_write_send(ctx, fd, buf, count, /*msghdr*/ NULL);
}

int syscall__probe_ret_sendto(struct pt_regs* ctx) {
  return probe_ret_write_send(ctx, kEventTypeSyscallSendEvent);
}

int syscall__probe_entry_sendmsg(struct pt_regs* ctx, int fd, const struct user_msghdr* msghdr) {
  return probe_entry_write_send(ctx, fd, /*buf*/ NULL, /*count*/ 0, msghdr);
}

int syscall__probe_ret_sendmsg(struct pt_regs* ctx) {
  return probe_ret_write_send(ctx, kEventTypeSyscallSendMsgEvent);
}

int syscall__probe_entry_recvmsg(struct pt_regs* ctx, int fd, struct user_msghdr* msghdr) {
  return probe_entry_read_recv(ctx, fd, /*buf*/ NULL, /*count*/ 0, msghdr);
}

int syscall__probe_ret_recvmsg(struct pt_regs* ctx) {
  return probe_ret_read_recv(ctx, kEventTypeSyscallRecvMsgEvent);
}

int syscall__probe_close(struct pt_regs* ctx, unsigned int fd) { return probe_close_impl(ctx, fd); }

// TODO(oazizi): Look into the following opens:
// 1) Should we trace sendmsg(), which is another syscall, but with a different interface?
// 2) Why does the syscall table only include sendto, while Linux source code and man page list both
// sendto and send? 3) What do we do when the sendto() is called with a dest_addr provided? I
// believe this overrides the conn_info.

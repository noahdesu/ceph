#include <errno.h>
#include "include/types.h"
#include "include/rados/librados.hpp"
#include "cls_zlog_bench_client.h"
#include "cls_zlog_bench_ops.h"

namespace zlog_bench {
  void cls_zlog_bench_append(librados::ObjectWriteOperation& op, uint64_t epoch,
      uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "append", in);
  }

  void cls_zlog_bench_append_plus_xtn(librados::ObjectWriteOperation& op, uint64_t epoch,
      uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "append_plus_xtn", in);
  }

  void cls_zlog_bench_append_sim_hdr_idx(librados::ObjectWriteOperation& op, uint64_t epoch,
      uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "append_sim_hdr_idx", in);
  }

  void cls_zlog_bench_append_wronly(librados::ObjectWriteOperation& op, uint64_t epoch,
      uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "append_wronly", in);
  }

  void cls_zlog_bench_append_init(librados::ObjectWriteOperation& op)
  {
    bufferlist in;
    op.exec("zlog_bench", "append_init", in);
  }

  void cls_zlog_bench_append_hdr_init(librados::ObjectWriteOperation& op)
  {
    bufferlist in;
    op.exec("zlog_bench", "append_hdr_init", in);
  }

  void cls_zlog_bench_stream_write_hdr_init(librados::ObjectWriteOperation& op)
  {
    bufferlist in;
    op.exec("zlog_bench", "stream_write_hdr_init", in);
  }

  void cls_zlog_bench_append_check_epoch(librados::ObjectWriteOperation& op,
      uint64_t epoch, uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "append_check_epoch", in);
  }

  void cls_zlog_bench_append_check_epoch_hdr(librados::ObjectWriteOperation& op,
      uint64_t epoch, uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "append_check_epoch_hdr", in);
  }

  void cls_zlog_bench_append_omap_index(librados::ObjectWriteOperation& op,
      uint64_t epoch, uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "append_omap_index", in);
  }

  void cls_zlog_bench_map_write_null(librados::ObjectWriteOperation& op,
      uint64_t epoch, uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "map_write_null", in);
  }

  void cls_zlog_bench_map_write_null_wronly(librados::ObjectWriteOperation& op,
      uint64_t epoch, uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "map_write_null_wronly", in);
  }

  void cls_zlog_bench_map_write_full(librados::ObjectWriteOperation& op,
      uint64_t epoch, uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "map_write_full", in);
  }

  void cls_zlog_bench_stream_write_null(librados::ObjectWriteOperation& op,
      uint64_t epoch, uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "stream_write_null", in);
  }

  void cls_zlog_bench_stream_write_null_sim_inline_idx(librados::ObjectWriteOperation& op,
      uint64_t epoch, uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "stream_write_null_sim_inline_idx", in);
  }

  void cls_zlog_bench_stream_write_null_sim_hdr_idx(librados::ObjectWriteOperation& op,
      uint64_t epoch, uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "stream_write_null_sim_hdr_idx", in);
  }

  void cls_zlog_bench_stream_write_null_wronly(librados::ObjectWriteOperation& op,
      uint64_t epoch, uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "stream_write_null_wronly", in);
  }

  void cls_zlog_bench_stream_write_full(librados::ObjectWriteOperation& op,
      uint64_t epoch, uint64_t position, ceph::bufferlist& data)
  {
    bufferlist in;
    cls_zlog_bench_append_op call;
    call.epoch = epoch;
    call.position = position;
    call.data = data;
    ::encode(call, in);
    op.exec("zlog_bench", "stream_write_full", in);
  }
}

/*
 * Optimization:
 *   - protobuf adapter for bufferlist
 *   - pull bulk data out of protobuf
 */
#include <sstream>
#include "include/rados/buffer.h"
#include "include/rados/objclass.h"
#include "zlog.pb.h"
#include "common.h"

CLS_VER(1, 0)
CLS_NAME(zlog)

enum EntryState {
  Unused  = 0,
  // all non-unused status must be non-zero
  Taken   = 1,
  Invalid = 2,
};

/*
 * Convert numeric value into zero-padded string for omap comparisons.
 */
static inline std::string u64tostr(uint64_t value, const std::string& prefix)
{
  std::stringstream ss;
  ss << prefix << std::setw(20) << std::setfill('0') << value;
  return ss.str();
}

static inline void calc_layout(uint64_t pos, uint32_t stripe_width,
    uint32_t entries_per_object, uint32_t entry_size,
    uint64_t *pobjectno, uint64_t *pslot_size, uint64_t *poffset)
{
  // logical layout
  const uint64_t stripe_num = pos / stripe_width;
  const uint64_t slot = stripe_num % entries_per_object;
  const uint64_t stripepos = pos % stripe_width;
  const uint64_t objectsetno = stripe_num / entries_per_object;
  const uint64_t objectno = objectsetno * stripe_width + stripepos;

  // physical layout
  const uint64_t slot_size = sizeof(uint8_t) + entry_size;
  const uint64_t offset = slot * slot_size;

  *pobjectno = objectno;
  *pslot_size = slot_size;
  *poffset = offset;
}

static int read_meta(cls_method_context_t hctx, zlog_proto::ObjectMeta& omd)
{
  ceph::bufferlist bl;
  int ret = cls_cxx_getxattr(hctx, "meta", &bl);
  if (ret < 0) {
    // expected to never read meta unless its been set
    if (ret == -ENODATA || ret == -ENOENT) {
      CLS_ERR("ERROR: read_meta(): entry or object not found");
      return -EIO;
    }
    return ret;
  }

  if (bl.length() == 0) {
    CLS_ERR("ERROR: read_meta(): no data");
    return -EIO;
  }

  if (!cls_zlog::decode(bl, &omd)) {
    CLS_ERR("ERROR: read_meta(): failed to decode meta data");
    return -EIO;
  }

  return 0;
}

static int init(cls_method_context_t hctx, ceph::bufferlist *in,
    ceph::bufferlist *out)
{
  zlog_proto::InitOp op;
  if (!cls_zlog::decode(*in, &op)) {
    CLS_ERR("ERROR: init(): failed to decode input");
    return -EINVAL;
  }

  // check if object exists
  int ret = cls_cxx_stat(hctx, NULL, NULL);
  if (ret < 0 && ret != -ENOENT) {
    CLS_ERR("ERROR: init(): stat failed: %d", ret);
    return ret;
  }

  // read (or initialize metadata)
  zlog_proto::ObjectMeta omd;
  if (ret == 0) {
    int ret = read_meta(hctx, omd);
    if (ret < 0) {
      CLS_ERR("ERROR: init(): could not read metadata");
      return ret;
    }
  } else {
    omd.mutable_params()->set_entry_size(op.params().entry_size());
    omd.mutable_params()->set_stripe_width(op.params().stripe_width());
    omd.mutable_params()->set_entries_per_object(op.params().entries_per_object());
    omd.set_object_id(op.object_id());

    ceph::bufferlist bl;
    cls_zlog::encode(bl, omd);
    ret = cls_cxx_setxattr(hctx, "meta", &bl);
    if (ret < 0) {
      CLS_ERR("ERROR: init(): failed to write metadata");
      return ret;
    }
  }

  if (omd.params().entry_size() == 0 ||
      omd.params().stripe_width() == 0 ||
      omd.params().entries_per_object() == 0) {
    CLS_ERR("ERROR: init(): invalid object metadata");
    return -EINVAL;
  }

  if (omd.params().entry_size() != op.params().entry_size() ||
      omd.params().stripe_width() != op.params().stripe_width() ||
      omd.params().entries_per_object() != op.params().entries_per_object() ||
      omd.object_id() != op.object_id()) {
    CLS_ERR("ERROR: init(): metadata mismatch");
    return -EINVAL;
  }

  return 0;
}

static int read(cls_method_context_t hctx, ceph::bufferlist *in,
    ceph::bufferlist *out)
{
  zlog_proto::ReadOp op;
  if (!cls_zlog::decode(*in, &op)) {
    CLS_ERR("ERROR: read(): failed to decode input");
    return -EINVAL;
  }

  // get object size
  uint64_t objsize = 0;
  int ret = cls_cxx_stat(hctx, &objsize, NULL);
  if (ret < 0) {
    if (ret == -ENOENT)
      CLS_LOG(10, "read(): object does not exist");
    else
      CLS_ERR("ERROR: read(): stat failed: %d", ret);
    return ret;
  }

  // read object metadata
  zlog_proto::ObjectMeta omd;
  ret = read_meta(hctx, omd);
  if (ret < 0) {
    CLS_ERR("ERROR: read(): failed to read metadata");
    return ret;
  }

  // defensive check: no param should be zero
  if (omd.params().entry_size() == 0 ||
      omd.params().stripe_width() == 0 ||
      omd.params().entries_per_object() == 0) {
    CLS_ERR("ERROR: read(): invalid object metadata");
    return -EIO;
  }

  uint64_t objectno;
  uint64_t slot_size;
  uint64_t offset;
  calc_layout(op.position(),
      omd.params().stripe_width(),
      omd.params().entries_per_object(),
      omd.params().entry_size(),
      &objectno,
      &slot_size,
      &offset);

  // defensive check: object identity match for this position?
  if (omd.object_id() != objectno) {
    CLS_ERR("ERROR: read(): wrong object target");
    return -EFAULT;
  }

  // read entry
  if ((offset + slot_size) <= objsize) {
    ceph::bufferlist bl;
    ret = cls_cxx_read(hctx, offset, slot_size, &bl);
    if (ret < 0) {
      CLS_ERR("ERROR: read(): failed to read entry");
      return ret;
    }

    if (bl.length() != slot_size) {
      CLS_ERR("ERROR: read(): partial entry read");
      return -EIO;
    }

    uint8_t hdr;
    memcpy(&hdr, bl.c_str(), sizeof(hdr));

    if (hdr == static_cast<uint8_t>(EntryState::Taken)) {
      CLS_LOG(10, "read(): reading entry");
      out->append(bl.c_str() + sizeof(hdr), slot_size - sizeof(hdr));
      return zlog_proto::ReadOp::OK;
    } else if (hdr == static_cast<uint8_t>(EntryState::Unused)) {
      CLS_LOG(10, "read(): entry not written");
      return zlog_proto::ReadOp::UNWRITTEN;
    } else if (hdr == static_cast<uint8_t>(EntryState::Invalid)) {
      CLS_LOG(10, "read(): invalid entry");
      return zlog_proto::ReadOp::INVALID;
    } else {
      CLS_ERR("ERROR: read(): unexpected status");
      return -EIO;
    }
  }

  CLS_LOG(10, "read(): entry not written (past eof)");
  return zlog_proto::ReadOp::UNWRITTEN;
}

static int write(cls_method_context_t hctx, ceph::bufferlist *in,
    ceph::bufferlist *out)
{
  zlog_proto::WriteOp op;
  if (!cls_zlog::decode(*in, &op)) {
    CLS_ERR("ERROR: write(): failed to decode input");
    return -EINVAL;
  }

  // get object size
  uint64_t objsize = 0;
  int ret = cls_cxx_stat(hctx, &objsize, NULL);
  if (ret < 0) {
    if (ret == -ENOENT)
      CLS_LOG(10, "write(): object does not exist");
    else
      CLS_ERR("ERROR: write(): stat failed: %d", ret);
    return ret;
  }

  // read object metadata
  zlog_proto::ObjectMeta omd;
  ret = read_meta(hctx, omd);
  if (ret < 0) {
    CLS_ERR("ERROR: write(): failed to read metadata");
    return ret;
  }

  // defensive check: no param should be zero
  if (omd.params().entry_size() == 0 ||
      omd.params().stripe_width() == 0 ||
      omd.params().entries_per_object() == 0) {
    CLS_ERR("ERROR: write(): invalid object metadata");
    return -EIO;
  }

  uint64_t objectno;
  uint64_t slot_size;
  uint64_t offset;
  calc_layout(op.position(),
      omd.params().stripe_width(),
      omd.params().entries_per_object(),
      omd.params().entry_size(),
      &objectno,
      &slot_size,
      &offset);

  // defensive check: object identity match for this position?
  if (omd.object_id() != objectno) {
    CLS_ERR("ERROR: write(): wrong object target");
    return -EFAULT;
  }

  // read entry header. correctness depends on zero'ed holes. alternatively,
  // we could "format" the object.
  uint8_t hdr = 0;
  if (offset < objsize) {
    ceph::bufferlist hdr_bl;
    ret = cls_cxx_read(hctx, offset, sizeof(hdr), &hdr_bl);
    if (ret < 0) {
      CLS_ERR("ERROR: write(): failed to read entry header");
      return ret;
    }

    if (hdr_bl.length() != sizeof(hdr)) {
      CLS_ERR("ERROR: write(): partial entry header read");
      return -EIO;
    }

    memcpy(&hdr, hdr_bl.c_str(), sizeof(hdr));
  }

  if (hdr) {
    CLS_LOG(10, "write(): entry already exists");
    return -EEXIST;
  }

  hdr = static_cast<uint8_t>(EntryState::Taken);
  assert(hdr);

  // prepare and write the log entry
  if ((op.data().size() + sizeof(hdr)) > slot_size) {
    CLS_ERR("ERROR: write(): entry too large");
    return -EFBIG;
  }

  ceph::bufferlist data(slot_size);
  data.append((char*)&hdr, sizeof(hdr));
  data.append(op.data().c_str(), op.data().size());
  const unsigned remaining = slot_size - data.length();
  if (remaining) {
    data.append_zero(remaining);
  }

  ret = cls_cxx_write(hctx, offset, data.length(), &data);
  if (ret < 0) {
    CLS_ERR("ERROR: write(): failed to write entry");
    return ret;
  }

  return 0;
}

static int invalidate(cls_method_context_t hctx, ceph::bufferlist *in,
    ceph::bufferlist *out)
{
  zlog_proto::InvalidateOp op;
  if (!cls_zlog::decode(*in, &op)) {
    CLS_ERR("ERROR: invalidate(): failed to decode input");
    return -EINVAL;
  }

  uint64_t objsize = 0;
  int ret = cls_cxx_stat(hctx, &objsize, NULL);
  if (ret < 0) {
    if (ret == -ENOENT)
      CLS_LOG(10, "invalidate(): object does not exist");
    else
      CLS_ERR("ERROR: invalidate(): stat failed: %d", ret);
    return ret;
  }

  // read object metadata
  zlog_proto::ObjectMeta omd;
  ret = read_meta(hctx, omd);
  if (ret < 0) {
    CLS_ERR("ERROR: invalidate(): failed to read metadata");
    return ret;
  }

  // defensive check: no param should be zero
  if (omd.params().entry_size() == 0 ||
      omd.params().stripe_width() == 0 ||
      omd.params().entries_per_object() == 0) {
    CLS_ERR("ERROR: invalidate(): invalid object metadata");
    return -EIO;
  }

  uint64_t objectno;
  uint64_t slot_size;
  uint64_t offset;
  calc_layout(op.position(),
      omd.params().stripe_width(),
      omd.params().entries_per_object(),
      omd.params().entry_size(),
      &objectno,
      &slot_size,
      &offset);

  // defensive check: object identity match for this position?
  if (omd.object_id() != objectno) {
    CLS_ERR("ERROR: invalidate(): wrong object target");
    return -EFAULT;
  }

  uint8_t hdr = 0;
  if (offset < objsize && !op.force()) {
    ceph::bufferlist hdr_bl;
    ret = cls_cxx_read(hctx, offset, sizeof(hdr), &hdr_bl);
    if (ret < 0) {
      CLS_ERR("ERROR: write(): failed to read entry header");
      return ret;
    }

    if (hdr_bl.length() != sizeof(hdr)) {
      CLS_ERR("ERROR: write(): partial entry header read");
      return -EIO;
    }

    memcpy(&hdr, hdr_bl.c_str(), sizeof(hdr));
  }

  if (hdr == static_cast<uint8_t>(EntryState::Invalid)) {
    CLS_LOG(10, "invalidate(): entry already invalid");
    return 0;
  }

  if (!hdr || op.force()) {
    hdr = static_cast<uint8_t>(EntryState::Invalid);

    ceph::bufferlist data;
    data.append((char*)&hdr, sizeof(hdr));
    if (offset >= objsize) {
      const unsigned remaining = slot_size - data.length();
      if (remaining) {
        data.append_zero(remaining);
      }
    }

    ret = cls_cxx_write(hctx, offset, data.length(), &data);
    if (ret < 0) {
      CLS_ERR("ERROR: invalidate(): failed to update entry");
      return ret;
    }

    return 0;
  }

  CLS_LOG(10, "invalidate(): entry is valid");
  return -EROFS;
}

static int view_init(cls_method_context_t hctx, ceph::bufferlist *in,
    ceph::bufferlist *out)
{
  zlog_proto::ViewInitOp op;
  if (!cls_zlog::decode(*in, &op)) {
    CLS_ERR("ERROR: view_init(): failed to decode input");
    return -EINVAL;
  }

  int ret = cls_cxx_stat(hctx, NULL, NULL);
  if (ret != -ENOENT) {
    if (ret >= 0) {
      CLS_ERR("ERROR: view_init(): object already exists");
      return -EEXIST;
    }
    CLS_ERR("ERROR: view_init(): stat error: %d", ret);
    return ret;
  }

  zlog_proto::View view;
  view.set_num_stripes(op.num_stripes());
  view.mutable_params()->set_entry_size(op.params().entry_size());
  view.mutable_params()->set_stripe_width(op.params().stripe_width());
  view.mutable_params()->set_entries_per_object(op.params().entries_per_object());

  if (view.num_stripes() == 0 ||
      view.params().entry_size() == 0 ||
      view.params().stripe_width() == 0 ||
      view.params().entries_per_object() == 0) {
    CLS_ERR("ERROR: view_init(): invalid view parameters");
    return -EINVAL;
  }

  const uint64_t epoch = 0;
  view.set_epoch(epoch);

  const std::string key = u64tostr(epoch, "view.epoch.");

  ceph::bufferlist bl;
  cls_zlog::encode(bl, view);
  ret = cls_cxx_map_set_val(hctx, key, &bl);
  if (ret < 0) {
    CLS_ERR("ERROR: view_init(): could not write view: %d", ret);
    return ret;
  }

  return 0;
}

static int view_read(cls_method_context_t hctx, ceph::bufferlist *in,
    ceph::bufferlist *out)
{
  zlog_proto::ViewReadOp op;
  if (!cls_zlog::decode(*in, &op)) {
    CLS_ERR("ERROR: view_read(): failed to decode input");
    return -EINVAL;
  }

  int ret = cls_cxx_stat(hctx, NULL, NULL);
  if (ret) {
    CLS_ERR("ERROR: view_read(): failed to stat view object: %d", ret);
    return ret;
  }

  zlog_proto::ViewReadOpReply reply;

  uint64_t epoch = op.min_epoch();
  while (true) {
    ceph::bufferlist bl;
    const std::string key = u64tostr(epoch, "view.epoch.");
    int ret = cls_cxx_map_get_val(hctx, key, &bl);
    if (ret < 0 && ret != -ENOENT) {
      CLS_ERR("ERROR: view_read(): failed to read view: %d", ret);
      return ret;
    }

    if (ret == -ENOENT)
      break;

    zlog_proto::View view;
    if (!cls_zlog::decode(bl, &view)) {
      CLS_ERR("ERROR: view_read(): failed to decode view");
      return -EIO;
    }

    auto reply_view = reply.add_views();
    *reply_view = view;

    epoch++;
  }

  if (reply.views_size() == 0) {
    CLS_ERR("ERROR: view_read(): no views found");
    return -EINVAL;
  }

  cls_zlog::encode(*out, reply);

  return 0;
}

CLS_INIT(zlog)
{
  CLS_LOG(0, "loading cls_zlog");

  cls_handle_t h_class;

  // log data object methods
  cls_method_handle_t h_init;
  cls_method_handle_t h_read;
  cls_method_handle_t h_write;
  cls_method_handle_t h_invalidate;

  // log metadata object methods
  cls_method_handle_t h_view_init;
  cls_method_handle_t h_view_read;


  cls_register("zlog", &h_class);

  cls_register_cxx_method(h_class, "init",
      CLS_METHOD_RD | CLS_METHOD_WR,
      init, &h_init);

  cls_register_cxx_method(h_class, "read",
      CLS_METHOD_RD,
      read, &h_read);

  cls_register_cxx_method(h_class, "write",
      CLS_METHOD_RD | CLS_METHOD_WR,
      write, &h_write);

  cls_register_cxx_method(h_class, "invalidate",
      CLS_METHOD_RD | CLS_METHOD_WR,
      invalidate, &h_invalidate);

  cls_register_cxx_method(h_class, "view_init",
      CLS_METHOD_RD | CLS_METHOD_WR,
      view_init, &h_view_init);

  cls_register_cxx_method(h_class, "view_read",
      CLS_METHOD_RD,
      view_read, &h_view_read);
}

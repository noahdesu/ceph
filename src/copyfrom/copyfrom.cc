#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <sstream>
#include <mutex>
#include <fstream>
#include <boost/program_options.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <fcntl.h>
#include "include/rados/librados.hpp"

namespace po = boost::program_options;

static inline uint64_t getns()
{
  struct timespec ts;
  int ret = clock_gettime(CLOCK_MONOTONIC, &ts);
  assert(ret == 0);
  return (((uint64_t)ts.tv_sec) * 1000000000ULL) + ts.tv_nsec;
}

class SourceManager {
 public:
  SourceManager(librados::IoCtx *ioctx, int num_objs)
    : ioctx_(ioctx), num_objs_(num_objs)
  {
    assert(num_objs_ > 0);
  }

  void gen_src_objects(const size_t obj_size) {
    // create random data to use for payloads
    const size_t rand_buf_size = 1ULL<<24;
    std::string rand_buf;
    rand_buf.reserve(rand_buf_size);
    std::ifstream ifs("/dev/urandom", std::ios::binary | std::ios::in);
    std::copy_n(std::istreambuf_iterator<char>(ifs),
        rand_buf_size, std::back_inserter(rand_buf));
    const char *rand_buf_raw = rand_buf.c_str();

    // grab random slices from the random bytes
    const size_t BLOCK_SIZE = 4096;
    std::default_random_engine generator;
    std::uniform_int_distribution<int> rand_dist;
    rand_dist = std::uniform_int_distribution<int>(0,
        rand_buf_size - BLOCK_SIZE - 1);

    const auto oids = src_oids();
    const size_t total = oids.size();
    size_t count = 0;

    for (auto oid : oids) {
      std::cout << "writing object "
        << ++count << "/" << total << ": "
        << oid.first << "\r" << std::flush;

      // quick out
      size_t check_size;
      int ret = ioctx_->stat(oid.first, &check_size, NULL);
      if (ret == 0 && check_size == obj_size)
        continue;

      // generate random object data
      ceph::bufferlist bl;
      size_t left = obj_size;
      while (left) {
        size_t copy_size = std::min(left, BLOCK_SIZE);
        size_t buf_offset = rand_dist(generator);
        bl.append(rand_buf_raw + buf_offset, copy_size);
        left -= copy_size;
      }
      assert(bl.length() == obj_size);

      ret = ioctx_->remove(oid.first);
      assert(ret == 0 || ret == -ENOENT);

      ret = ioctx_->write_full(oid.first, bl);
      assert(ret == 0);
    }

    std::cout << std::endl;
  }

  std::vector<std::pair<std::string, size_t>> src_oids(size_t want_size) {
    return src_oids(want_size, true);
  }

  std::vector<std::pair<std::string, size_t>> src_oids() {
    return src_oids(0, false);
  }

 private:
  std::vector<std::pair<std::string, size_t>> src_oids(size_t want_size,
      bool verify) {

    assert(num_objs_ > 0);

    if (verify) {
      std::cout << "verifying " << num_objs_
        << " objects (size=" << want_size
        << ")... " << std::flush;
    }

    std::vector<std::pair<std::string, size_t>> oids;
    for (int i = 0; i < num_objs_; i++) {
      size_t size = 0;
      const std::string oid = make_oid(i);
      if (verify) {
        int ret = ioctx_->stat(oid, &size, NULL);
        assert(ret == 0);
        assert(size == want_size);
      }
      oids.push_back(std::make_pair(oid, size));
    }

    if (verify)
      std::cout << "completed!" << std::endl << std::flush;

    return oids;
  }

  std::string make_oid(int i) {
    std::stringstream ss;
    ss << "copyfrom.src." << i;
    return ss.str();
  }

  librados::IoCtx *ioctx_;
  const int num_objs_;
};

class CopyWorkload {
 public:
  CopyWorkload(librados::IoCtx *ioctx, SourceManager *src, int qdepth,
      size_t obj_size) :
    ioctx_(ioctx), src_oids_(src->src_oids(obj_size)),
    num_objs_(src_oids_.size()), qdepth_(qdepth)
  {
    assert(num_objs_ > 0);
    assert(qdepth_ > 0);
  }

  virtual ~CopyWorkload() {}

  void run(const std::string& stats_fn) {
    // new name for this run
    set_dst_uuid();

    // divide work by src_oids_ index
    oid_index_ = 0;

    // verify dst oids are new
    verify_dst_oids();

    uint64_t ver = ioctx_->get_last_version();

    bool stop = false;
    std::thread monitor(&CopyWorkload::monitor, this, &stop);

    // spawn workers
    std::vector<std::thread> workers;
    for (int i = 0; i < qdepth_; i++) {
      workers.push_back(
          std::thread(&CopyWorkload::worker, this, ver));
    }

    // wait for finish
    std::for_each(workers.begin(), workers.end(),
        [](std::thread& t) { t.join(); });

    stop = true;
    monitor.join();

    const size_t num_dst_oids = dst_oids_.size();
    size_t num_dst_oids_removed = 0;
    for (auto oid : dst_oids_) {
      int ret = ioctx_->remove(oid);
      assert(ret == 0);

      num_dst_oids_removed++;
      std::cout << "cleaning up "
        << num_dst_oids_removed << "/" << num_dst_oids
        << " objects\r" << std::flush;
    }
    std::cout << std::endl << std::flush;

    int fd = -1;
    if (stats_fn == "-") {
      fd = 0;
    } else if (!stats_fn.empty()) {
      fd = open(stats_fn.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0444);
      assert(fd != -1);
    }

    std::lock_guard<std::mutex> l(lock_);

    const std::string mode = mode_name();

    if (fd != -1) {
      dprintf(fd, "mode,runid,qdepth,objsize,begin,end\n");
      for (auto op_stats : op_stats_) {
        dprintf(fd, "%s,%s,%d,%lu,%llu,%llu\n",
            mode.c_str(), dst_uuid_.c_str(),
            qdepth_, op_stats.bytes,
            (unsigned long long)op_stats.begin,
            (unsigned long long)op_stats.end);
      }

      fsync(fd);

      if (fd != 0)
        close(fd);
    }
  }

 protected:
  virtual void handle_copy(const std::string& src_oid,
      const std::string& dst_oid, uint64_t src_ver) = 0;

  virtual std::string mode_name() const = 0;

 private:
  void monitor(bool *stop) {
    time_t start = time(NULL);
    while (!*stop) {
      sleep(1);

      time_t dur = time(NULL) - start;
      // oid_index_ can go above num_objs_ but the threads that cause that
      // bump notice the job is completed and exit. we cap here just to make
      // the status output make sense.
      unsigned completed = std::min(oid_index_ + 1, (unsigned int)num_objs_);
      double rate = (double)completed / (double)dur;
      int est = (double)(num_objs_ - completed) / rate;
      est = std::max(0, est);

      std::cout << mode_name() << " copy progress: "
        << completed << "/" << num_objs_
        << " est. secs: " << est << "\r" << std::flush;
    }
    std::cout << std::endl << std::flush;
  }

  // generate a unique dst_uuid uuid
  void set_dst_uuid() {
    boost::uuids::uuid uuid =
      boost::uuids::random_generator()();
    std::stringstream ss;
    ss << uuid;
    dst_uuid_ = ss.str();
  }

  void verify_dst_oids() const {
    assert(!src_oids_.empty());
    for (auto src_oid : src_oids_) {
      std::string dst_oid = make_dst_oid(src_oid.first);
      int ret = ioctx_->stat(dst_oid, NULL, NULL);
      assert(ret == -ENOENT);
    }
  }

  std::string make_dst_oid(const std::string& src_oid) const {
    std::stringstream ss;
    ss << "copyfrom.dst." << dst_uuid_ << "." << src_oid;
    return ss.str();
  }

  void worker(uint64_t ver) {
    std::vector<op_stat> op_stats;
    std::vector<std::string> dst_oids;

    for (;;) {
      size_t idx = oid_index_.fetch_add(1);
      if (idx >= num_objs_)
        break;

      const auto src_oid = src_oids_[idx];
      std::string dst_oid = make_dst_oid(src_oid.first);

      struct op_stat op;

      op.begin = getns();
      handle_copy(src_oid.first, dst_oid, ver);
      op.end = getns();

      op.bytes = src_oid.second;

      op_stats.push_back(op);
      dst_oids.push_back(dst_oid);
    }

    std::lock_guard<std::mutex> l(lock_);

    op_stats_.insert(std::end(op_stats_),
        std::begin(op_stats), std::end(op_stats));

    dst_oids_.insert(std::end(dst_oids_),
        std::begin(dst_oids), std::end(dst_oids));
  }

 protected:
  librados::IoCtx *ioctx_;

 private:
  const std::vector<std::pair<std::string, size_t>> src_oids_;
  const size_t num_objs_;
  const int qdepth_;
  std::string dst_uuid_;
  std::atomic_uint oid_index_;
  std::mutex lock_;

  struct op_stat {
    uint64_t begin;
    uint64_t end;
    size_t bytes;
  };

  std::vector<op_stat> op_stats_;
  std::vector<std::string> dst_oids_;
};

class ClientCopyWorkload : public CopyWorkload {
 public:
  using CopyWorkload::CopyWorkload;

 protected:
  virtual void handle_copy(const std::string& src_oid,
      const std::string& dst_oid, uint64_t src_ver) {

    ceph::bufferlist bl;
    int ret = ioctx_->read(src_oid, bl, 0, 0);
    assert(ret > 0);
    assert(bl.length() > 0);

    ret = ioctx_->write_full(dst_oid, bl);
    assert(ret == 0);
  }

  virtual std::string mode_name() const {
    return "client";
  }
};

class ServerCopyWorkload : public CopyWorkload {
 public:
  using CopyWorkload::CopyWorkload;

 protected:
  virtual void handle_copy(const std::string& src_oid,
      const std::string& dst_oid, uint64_t src_ver) {

    librados::ObjectWriteOperation op;
    op.copy_from(src_oid, *ioctx_, src_ver);
    int ret = ioctx_->operate(dst_oid, &op);
    assert(ret == 0);
  }

  virtual std::string mode_name() const {
    return "server";
  }
};

int main(int argc, char **argv)
{
  size_t num_objs;
  size_t obj_size;
  std::string pool;
  bool gendata;
  int qdepth;
  bool copy_client;
  bool copy_server;
  std::string stats_fn;

  po::options_description gen_opts("General options");
  gen_opts.add_options()
    ("help,h", "show help message")
    ("pool", po::value<std::string>(&pool)->required(), "rados pool")
    ("num-objs", po::value<size_t>(&num_objs)->default_value(0), "number of objects")
    ("stats-fn", po::value<std::string>(&stats_fn)->default_value(""), "stats filename")
  ;

  po::options_description copy_opts("Copy workload options");
  copy_opts.add_options()
    ("copy-client", po::bool_switch(&copy_client)->default_value(false), "client copy mode")
    ("copy-server", po::bool_switch(&copy_server)->default_value(false), "server copy mode")
    ("qdepth", po::value<int>(&qdepth)->default_value(1), "queue depth")
  ;

  po::options_description datagen_opts("Source data generator options");
  datagen_opts.add_options()
    ("gendata", po::bool_switch(&gendata)->default_value(false), "generate source data")
    ("obj-size", po::value<size_t>(&obj_size)->default_value(0), "size of each object")
  ;

  po::options_description all_opts("Allowed options");
  all_opts.add(gen_opts).add(datagen_opts).add(copy_opts);

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, all_opts), vm);

  if (vm.count("help")) {
    std::cout << all_opts << std::endl;
    return 1;
  }

  po::notify(vm);

  bool copy_mode = copy_client || copy_server;

  if (copy_mode && gendata) {
    std::cerr << "copy mode and gendata mode are exclusive" << std::endl;
    exit(1);
  }

  if (copy_client && copy_server) {
    std::cerr << "copy modes are exclusive" << std::endl;
    exit(1);
  }

  if (copy_mode || gendata) {
    if (num_objs <= 0) {
      std::cerr << "positive --num-objs value required" << std::endl;
      exit(1);
    }

    if (obj_size <= 0) {
      std::cerr << "postive --obj-size value required" << std::endl;
      exit(1);
    }
  }

  if (!copy_mode && !gendata) {
    std::cerr << "no workload mode specified" << std::endl;
    exit(1);
  }

  // connect to rados
  librados::Rados cluster;
  cluster.init(NULL);
  cluster.conf_read_file(NULL);
  int ret = cluster.connect();
  assert(ret == 0);

  // open pool i/o context
  librados::IoCtx ioctx;
  ret = cluster.ioctx_create(pool.c_str(), ioctx);
  assert(ret == 0);

  SourceManager src_mgr(&ioctx, num_objs);

  if (gendata) {
    src_mgr.gen_src_objects(obj_size);

  } else if (copy_client) {
    ClientCopyWorkload workload(&ioctx, &src_mgr, qdepth, obj_size);
    workload.run(stats_fn);

  } else if (copy_server) {
    ServerCopyWorkload workload(&ioctx, &src_mgr, qdepth, obj_size);
    workload.run(stats_fn);
  }

  ioctx.close();
  cluster.shutdown();

  return 0;
}

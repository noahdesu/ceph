#include <sstream>
#include <boost/program_options.hpp>
#include <limits>
#include <sstream>
#include "include/types.h"
#include <common/Clock.h>
#include "include/rados/librados.hpp"

namespace po = boost::program_options;

static const char *empty_script =
"function lua_empty(input, output)\n"
"end\n"
"cls.register(lua_empty)\n";

static const char *write_input_script =
"function lua_write_input(input, output)\n"
"cls.write(0, #input, input);\n"
"end\n"
"cls.register(lua_write_input)\n";

static std::string cmd(const std::string script_name, std::string pool)
{
  const char *script = "";
  if (script_name == "lua_empty")
    script = empty_script;
  else if (script_name == "lua_write_input")
    script = write_input_script;

  return std::string("{\"var\": \"lua_class\", \"prefix\": \"osd pool set\", ") +
         std::string("\"val\": \"") + std::string(script) + std::string("\",") +
         std::string("\"pool\": \"" + pool + "\"}");
}

static void install_script(librados::Rados& cluster, std::string pool, std::string method)
{
  ceph::bufferlist inbl, outbl;
  std::string outstring;
  int ret = cluster.mon_command(cmd(method, pool), inbl, &outbl, &outstring);
  assert(ret == 0);
  sleep(2);
}

int main(int argc, char **argv)
{
  std::string pool, cls, method;
  unsigned input_size;
  bool lua_cost;
  int ops;
  std::string obj;

  po::options_description desc("Allowed options");
  desc.add_options()
    ("pool", po::value<std::string>(&pool)->required(), "Pool name")
    ("cls", po::value<std::string>(&cls)->required(), "Class name")
    ("method", po::value<std::string>(&method)->required(), "Method name")
    ("isize", po::value<unsigned>(&input_size)->default_value(0), "Input size")
    ("lua_cost", po::value<bool>(&lua_cost)->default_value(false), "Print OSD lua cost")
    ("ops", po::value<int>(&ops)->default_value(0), "Num ops")
    ("obj", po::value<std::string>(&obj)->required(), "Object name")
  ;

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  // connect to rados
  librados::Rados cluster;
  cluster.init(NULL);
  cluster.conf_read_file(NULL);
  cluster.conf_parse_env(NULL);
  int ret = cluster.connect();
  assert(ret == 0);

  // open pool i/o context
  librados::IoCtx ioctx;
  ret = cluster.ioctx_create(pool.c_str(), ioctx);
  assert(ret == 0);

  // install script based on the method name that will be called.
  install_script(cluster, pool, method);

  // setup a buffer sized for the requested input size
  ceph::bufferlist inbl;
  if (input_size > 0) {
    char data[input_size];
    for (unsigned i = 0; i < input_size; i++)
      data[i] = (char)rand();
    inbl.append(data, input_size);
  }

  /*
   * Measuring the Lua overhead in the OSD do_ops measure the CPU overhead...
   * it doesn't actually take into account the IO costs. So we can compare
   * overhead of Lua on the OSD and the actual I/O costs. For the I/O costs we
   * compare latency observed by client.
   */
  if (lua_cost) {
    assert(cls == "lua");
    cls = "lua_cost";
  }

  for (int i = 0; 1; i++) {
    ceph::bufferlist outbl;
    assert(inbl.length() == input_size);
    utime_t start = ceph_clock_now(NULL);
    ret = ioctx.exec(obj, cls.c_str(), method.c_str(), inbl, outbl);
    assert(ret == 0);

    utime_t dur = ceph_clock_now(NULL) - start;
    std::cout << i << ": " << dur.to_nsec() << std::endl;

    if (ops && i >= ops)
      break;
  }

  ioctx.close();
  cluster.shutdown();

  return 0;
}

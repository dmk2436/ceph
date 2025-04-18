// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/types.h"
#include "cls/log/cls_log_types.h"
#include "cls/log/cls_log_client.h"

#include "common/ceph_time.h"
#include "global/global_context.h"

#include "gtest/gtest.h"
#include "test/librados/test_cxx.h"

#include <errno.h>
#include <string>
#include <vector>

using namespace std;
using namespace std::literals;

using ceph::real_time;
using ceph::real_clock;

/// creates a temporary pool and initializes an IoCtx for each test
class cls_log : public ::testing::Test {
  librados::Rados rados;
  std::string pool_name;
 protected:
  librados::IoCtx ioctx;

  void SetUp() {
    pool_name = get_temp_pool_name();
    /* create pool */
    ASSERT_EQ("", create_one_pool_pp(pool_name, rados));
    ASSERT_EQ(0, rados.ioctx_create(pool_name.c_str(), ioctx));
  }
  void TearDown() {
    /* remove pool */
    ioctx.close();
    ASSERT_EQ(0, destroy_one_pool_pp(pool_name, rados));
  }
};

static int read_bl(bufferlist& bl, int *i)
{
  auto iter = bl.cbegin();

  try {
    decode(*i, iter);
  } catch (buffer::error& err) {
    std::cout << "failed to decode buffer" << std::endl;
    return -EIO;
  }

  return 0;
}

void add_log(librados::ObjectWriteOperation *op, real_time timestamp, string& section, string&name, int i)
{
  bufferlist bl;
  encode(i, bl);

  cls_log_add(*op, timestamp, section, name, bl);
}


string get_name(int i)
{
  string name_prefix = "data-source";

  char buf[16];
  snprintf(buf, sizeof(buf), "%d", i);
  return name_prefix + buf;
}

void generate_log(librados::IoCtx& ioctx, string& oid, int max, real_time start_time, bool modify_time)
{
  string section = "global";

  librados::ObjectWriteOperation op;

  int i;

  for (i = 0; i < max; i++) {
    // coverity[store_truncates_time_t:SUPPRESS]
    auto ts = start_time;
    if (modify_time)
      ts += i * 1s;

    string name = get_name(i);

    add_log(&op, ts, section, name, i);
  }

  ASSERT_EQ(0, ioctx.operate(oid, &op));
}

real_time get_time(real_time start_time, int i, bool modify_time)
{
  // coverity[store_truncates_time_t:SUPPRESS]
  return modify_time ? start_time + (i * 1s) : start_time;
}

void check_entry(cls::log::entry& entry, real_time start_time, int i, bool modified_time)
{
  string section = "global";
  string name = get_name(i);
  auto ts = get_time(start_time, i, modified_time);

  ASSERT_EQ(section, entry.section);
  ASSERT_EQ(name, entry.name);
  ASSERT_EQ(ts, entry.timestamp);
}

static int log_list(librados::IoCtx& ioctx, const std::string& oid,
                    real_time from, real_time to,
                    const string& in_marker, int max_entries,
                    vector<cls::log::entry>& entries,
                    string *out_marker, bool *truncated)
{
  librados::ObjectReadOperation rop;
  cls_log_list(rop, from, to, in_marker, max_entries,
               entries, out_marker, truncated);
  bufferlist obl;
  return ioctx.operate(oid, &rop, &obl);
}

static int log_list(librados::IoCtx& ioctx, const std::string& oid,
                    real_time from, real_time to, int max_entries,
                    vector<cls::log::entry>& entries, bool *truncated)
{
  std::string marker;
  return log_list(ioctx, oid, from, to, marker, max_entries,
                  entries, &marker, truncated);
}

static int log_list(librados::IoCtx& ioctx, const std::string& oid,
                    vector<cls::log::entry>& entries)
{
  real_time from, to;
  bool truncated{false};
  return log_list(ioctx, oid, from, to, 0, entries, &truncated);
}

TEST_F(cls_log, test_log_add_same_time)
{
  /* add chains */
  string oid = "obj";

  /* create object */
  ASSERT_EQ(0, ioctx.create(oid, true));

  /* generate log */
  auto start_time = real_clock::now();
  auto to_time = get_time(start_time, 1, true);
  generate_log(ioctx, oid, 10, start_time, false);

  vector<cls::log::entry> entries;
  bool truncated;

  /* check list */
  {
    ASSERT_EQ(0, log_list(ioctx, oid, start_time, to_time, 0,
                          entries, &truncated));
    ASSERT_EQ(10, (int)entries.size());
    ASSERT_EQ(0, (int)truncated);
  }
  vector<cls::log::entry>::iterator iter;

  /* need to sort returned entries, all were using the same time as key */
  map<int, cls::log::entry> check_ents;

  for (iter = entries.begin(); iter != entries.end(); ++iter) {
    cls::log::entry& entry = *iter;

    int num;
    ASSERT_EQ(0, read_bl(entry.data, &num));

    check_ents[num] = entry;
  }

  ASSERT_EQ(10, (int)check_ents.size());

  map<int, cls::log::entry>::iterator ei;

  /* verify entries are as expected */

  int i;

  for (i = 0, ei = check_ents.begin(); i < 10; i++, ++ei) {
    cls::log::entry& entry = ei->second;

    ASSERT_EQ(i, ei->first);
    check_entry(entry, start_time, i, false);
  }

  /* check list again, now want to be truncated*/
  {
    ASSERT_EQ(0, log_list(ioctx, oid, start_time, to_time, 1,
                          entries, &truncated));
    ASSERT_EQ(1, (int)entries.size());
    ASSERT_EQ(1, (int)truncated);
  }
}

TEST_F(cls_log, test_log_add_different_time)
{
  /* add chains */
  string oid = "obj";

  /* create object */
  ASSERT_EQ(0, ioctx.create(oid, true));

  /* generate log */
  auto start_time = real_clock::now();
  generate_log(ioctx, oid, 10, start_time, true);

  vector<cls::log::entry> entries;
  bool truncated;

  auto to_time = start_time + (10 * 1s);

  {
    /* check list */
    ASSERT_EQ(0, log_list(ioctx, oid, start_time, to_time, 0,
                          entries, &truncated));
    ASSERT_EQ(10, (int)entries.size());
    ASSERT_EQ(0, (int)truncated);
  }

  vector<cls::log::entry>::iterator iter;

  /* returned entries should be sorted by time */
  map<int, cls::log::entry> check_ents;

  int i;

  for (i = 0, iter = entries.begin(); iter != entries.end(); ++iter, ++i) {
    cls::log::entry& entry = *iter;

    int num;

    ASSERT_EQ(0, read_bl(entry.data, &num));

    ASSERT_EQ(i, num);

    check_entry(entry, start_time, i, true);
  }

  /* check list again with shifted time */
  {
    auto next_time = get_time(start_time, 1, true);
    ASSERT_EQ(0, log_list(ioctx, oid, next_time, to_time, 0,
                          entries, &truncated));
    ASSERT_EQ(9u, entries.size());
    ASSERT_FALSE(truncated);
  }

  string marker;
  i = 0;
  do {
    string old_marker = std::move(marker);
    ASSERT_EQ(0, log_list(ioctx, oid, start_time, to_time, old_marker, 1,
                          entries, &marker, &truncated));
    ASSERT_NE(old_marker, marker);
    ASSERT_EQ(1, (int)entries.size());

    ++i;
    ASSERT_GE(10, i);
  } while (truncated);

  ASSERT_EQ(10, i);
}

int do_log_trim(librados::IoCtx& ioctx, const std::string& oid,
                const std::string& from_marker, const std::string& to_marker)
{
  librados::ObjectWriteOperation op;
  cls_log_trim(op, {}, {}, from_marker, to_marker);
  return ioctx.operate(oid, &op);
}

int do_log_trim(librados::IoCtx& ioctx, const std::string& oid,
                real_time from_time, real_time to_time)
{
  librados::ObjectWriteOperation op;
  cls_log_trim(op, from_time, to_time, "", "");
  return ioctx.operate(oid, &op);
}

TEST_F(cls_log, trim_by_time)
{
  /* add chains */
  string oid = "obj";

  /* create object */
  ASSERT_EQ(0, ioctx.create(oid, true));

  /* generate log */
  auto start_time = real_clock::now();
  generate_log(ioctx, oid, 10, start_time, true);

  vector<cls::log::entry> entries;
  bool truncated;

  /* check list */

  /* trim */
  auto to_time = get_time(start_time, 10, true);

  for (int i = 0; i < 10; i++) {
    auto trim_time = get_time(start_time, i, true);

    real_time zero_time;

    ASSERT_EQ(0, do_log_trim(ioctx, oid, zero_time, trim_time));
    ASSERT_EQ(-ENODATA, do_log_trim(ioctx, oid, zero_time, trim_time));

    ASSERT_EQ(0, log_list(ioctx, oid, start_time, to_time, 0,
                          entries, &truncated));
    ASSERT_EQ(9u - i, entries.size());
    ASSERT_FALSE(truncated);
  }
}

TEST_F(cls_log, trim_by_marker)
{
  string oid = "obj";
  ASSERT_EQ(0, ioctx.create(oid, true));

  auto start_time = real_clock::now();
  generate_log(ioctx, oid, 10, start_time, true);

  std::vector<cls::log::entry> log1;
  {
    vector<cls::log::entry> entries;
    ASSERT_EQ(0, log_list(ioctx, oid, entries));
    ASSERT_EQ(10u, entries.size());

    log1.assign(std::make_move_iterator(entries.begin()),
                std::make_move_iterator(entries.end()));
  }
  // trim front of log
  {
    const std::string from = "";
    const std::string to = log1[0].id;
    ASSERT_EQ(0, do_log_trim(ioctx, oid, from, to));
    vector<cls::log::entry> entries;
    ASSERT_EQ(0, log_list(ioctx, oid, entries));
    ASSERT_EQ(9u, entries.size());
    EXPECT_EQ(log1[1].id, entries.begin()->id);
    ASSERT_EQ(-ENODATA, do_log_trim(ioctx, oid, from, to));
  }
  // trim back of log
  {
    const std::string from = log1[8].id;
    const std::string to = "9";
    ASSERT_EQ(0, do_log_trim(ioctx, oid, from, to));
    vector<cls::log::entry> entries;
    ASSERT_EQ(0, log_list(ioctx, oid, entries));
    ASSERT_EQ(8u, entries.size());
    EXPECT_EQ(log1[8].id, entries.rbegin()->id);
    ASSERT_EQ(-ENODATA, do_log_trim(ioctx, oid, from, to));
  }
  // trim a key from the middle
  {
    const std::string from = log1[3].id;
    const std::string to = log1[4].id;
    ASSERT_EQ(0, do_log_trim(ioctx, oid, from, to));
    vector<cls::log::entry> entries;
    ASSERT_EQ(0, log_list(ioctx, oid, entries));
    ASSERT_EQ(7u, entries.size());
    ASSERT_EQ(-ENODATA, do_log_trim(ioctx, oid, from, to));
  }
  // trim full log
  {
    const std::string from = "";
    const std::string to = "9";
    ASSERT_EQ(0, do_log_trim(ioctx, oid, from, to));
    vector<cls::log::entry> entries;
    ASSERT_EQ(0, log_list(ioctx, oid, entries));
    ASSERT_EQ(0u, entries.size());
    ASSERT_EQ(-ENODATA, do_log_trim(ioctx, oid, from, to));
  }
}

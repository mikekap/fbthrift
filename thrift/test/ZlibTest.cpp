/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#define __STDC_LIMIT_MACROS
#define __STDC_FORMAT_MACROS

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // needed for getopt_long
#endif

#include <stdint.h>
#include <inttypes.h>
#include <getopt.h>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <functional>

#include <boost/random.hpp>
#include <boost/shared_array.hpp>
#include <boost/test/unit_test.hpp>

#include <thrift/lib/cpp/transport/TBufferTransports.h>
#include <thrift/lib/cpp/transport/TZlibTransport.h>

using std::string;
using namespace boost;
using namespace apache::thrift::transport;

boost::mt19937 rng;

/*
 * Utility code
 */

class SizeGenerator {
 public:
  virtual ~SizeGenerator() {}
  virtual unsigned int getSize() = 0;
};

class ConstantSizeGenerator : public SizeGenerator {
 public:
  explicit ConstantSizeGenerator(unsigned int value) : value_(value) {}
  virtual unsigned int getSize() {
    return value_;
  }

 private:
  unsigned int value_;
};

class LogNormalSizeGenerator : public SizeGenerator {
 public:
  LogNormalSizeGenerator(double mean, double std_dev) :
      gen_(rng, lognormal_distribution<double>(mean, std_dev)) {}

  virtual unsigned int getSize() {
    // Loop until we get a size of 1 or more
    while (true) {
      unsigned int value = static_cast<unsigned int>(gen_());
      if (value >= 1) {
        return value;
      }
    }
  }

 private:
  variate_generator< mt19937, lognormal_distribution<double> > gen_;
};

uint8_t* gen_uniform_buffer(uint32_t buf_len, uint8_t c) {
  uint8_t* buf = new uint8_t[buf_len];
  memset(buf, c, buf_len);
  return buf;
}

uint8_t* gen_compressible_buffer(uint32_t buf_len) {
  uint8_t* buf = new uint8_t[buf_len];

  // Generate small runs of alternately increasing and decreasing bytes
  boost::uniform_smallint<uint32_t> run_length_distribution(1, 64);
  boost::uniform_smallint<uint8_t> byte_distribution(0, UINT8_MAX);
  boost::variate_generator< boost::mt19937, boost::uniform_smallint<uint8_t> >
    byte_generator(rng, byte_distribution);
  boost::variate_generator< boost::mt19937, boost::uniform_smallint<uint32_t> >
    run_len_generator(rng, run_length_distribution);

  uint32_t idx = 0;
  int8_t step = 1;
  while (idx < buf_len) {
    uint32_t run_length = run_len_generator();
    if (idx + run_length > buf_len) {
      run_length = buf_len - idx;
    }

    uint8_t byte = byte_generator();
    for (uint32_t n = 0; n < run_length; ++n) {
      buf[idx] = byte;
      ++idx;
      byte += step;
    }

    step *= -1;
  }

  return buf;
}

uint8_t* gen_random_buffer(uint32_t buf_len) {
  uint8_t* buf = new uint8_t[buf_len];

  boost::uniform_smallint<uint8_t> distribution(0, UINT8_MAX);
  boost::variate_generator< boost::mt19937, boost::uniform_smallint<uint8_t> >
    generator(rng, distribution);

  for (uint32_t n = 0; n < buf_len; ++n) {
    buf[n] = generator();
  }

  return buf;
}

/*
 * Test functions
 */

void test_write_then_read(const uint8_t* buf, uint32_t buf_len) {
  std::shared_ptr<TMemoryBuffer> membuf(new TMemoryBuffer());
  std::shared_ptr<TZlibTransport> zlib_trans(new TZlibTransport(membuf));
  zlib_trans->write(buf, buf_len);
  zlib_trans->finish();

  boost::shared_array<uint8_t> mirror(new uint8_t[buf_len]);
  uint32_t got = zlib_trans->readAll(mirror.get(), buf_len);
  BOOST_REQUIRE_EQUAL(got, buf_len);
  BOOST_CHECK_EQUAL(memcmp(mirror.get(), buf, buf_len), 0);
  zlib_trans->verifyChecksum();
}

void test_separate_checksum(const uint8_t* buf, uint32_t buf_len) {
  // This one is tricky.  I separate the last byte of the stream out
  // into a separate crbuf_.  The last byte is part of the checksum,
  // so the entire read goes fine, but when I go to verify the checksum
  // it isn't there.  The original implementation complained that
  // the stream was not complete.  I'm about to go fix that.
  // It worked.  Awesome.
  std::shared_ptr<TMemoryBuffer> membuf(new TMemoryBuffer());
  std::shared_ptr<TZlibTransport> zlib_trans(new TZlibTransport(membuf));
  zlib_trans->write(buf, buf_len);
  zlib_trans->finish();
  string tmp_buf;
  membuf->appendBufferToString(tmp_buf);
  zlib_trans.reset(new TZlibTransport(membuf,
                                      TZlibTransport::DEFAULT_URBUF_SIZE,
                                      tmp_buf.length()-1));

  boost::shared_array<uint8_t> mirror(new uint8_t[buf_len]);
  uint32_t got = zlib_trans->readAll(mirror.get(), buf_len);
  BOOST_REQUIRE_EQUAL(got, buf_len);
  BOOST_CHECK_EQUAL(memcmp(mirror.get(), buf, buf_len), 0);
  zlib_trans->verifyChecksum();
}

void test_incomplete_checksum(const uint8_t* buf, uint32_t buf_len) {
  // Make sure we still get that "not complete" error if
  // it really isn't complete.
  std::shared_ptr<TMemoryBuffer> membuf(new TMemoryBuffer());
  std::shared_ptr<TZlibTransport> zlib_trans(new TZlibTransport(membuf));
  zlib_trans->write(buf, buf_len);
  zlib_trans->finish();
  string tmp_buf;
  membuf->appendBufferToString(tmp_buf);
  tmp_buf.erase(tmp_buf.length() - 1);
  membuf->resetBuffer(const_cast<uint8_t*>(
                        reinterpret_cast<const uint8_t*>(tmp_buf.data())),
                      tmp_buf.length());

  boost::shared_array<uint8_t> mirror(new uint8_t[buf_len]);
  uint32_t got = zlib_trans->readAll(mirror.get(), buf_len);
  BOOST_REQUIRE_EQUAL(got, buf_len);
  BOOST_CHECK_EQUAL(memcmp(mirror.get(), buf, buf_len), 0);
  try {
    zlib_trans->verifyChecksum();
    BOOST_ERROR("verifyChecksum() did not report an error");
  } catch (TTransportException& ex) {
    BOOST_CHECK_EQUAL(ex.getType(), TTransportException::CORRUPTED_DATA);
  }
}

void test_read_write_mix(const uint8_t* buf, uint32_t buf_len,
                         const std::shared_ptr<SizeGenerator>& write_gen,
                         const std::shared_ptr<SizeGenerator>& read_gen) {
  // Try it with a mix of read/write sizes.
  std::shared_ptr<TMemoryBuffer> membuf(new TMemoryBuffer());
  std::shared_ptr<TZlibTransport> zlib_trans(new TZlibTransport(membuf));
  unsigned int tot;

  tot = 0;
  while (tot < buf_len) {
    uint32_t write_len = write_gen->getSize();
    if (tot + write_len > buf_len) {
      write_len = buf_len - tot;
    }
    zlib_trans->write(buf + tot, write_len);
    tot += write_len;
  }

  zlib_trans->finish();

  tot = 0;
  boost::shared_array<uint8_t> mirror(new uint8_t[buf_len]);
  while (tot < buf_len) {
    uint32_t read_len = read_gen->getSize();
    uint32_t expected_read_len = read_len;
    if (tot + read_len > buf_len) {
      expected_read_len = buf_len - tot;
    }
    uint32_t got = zlib_trans->read(mirror.get() + tot, read_len);
    BOOST_REQUIRE_LE(got, expected_read_len);
    BOOST_REQUIRE_NE(got, 0);
    tot += got;
  }

  BOOST_CHECK_EQUAL(memcmp(mirror.get(), buf, buf_len), 0);
  zlib_trans->verifyChecksum();
}

void test_invalid_checksum(const uint8_t* buf, uint32_t buf_len) {
  // Verify checksum checking.
  std::shared_ptr<TMemoryBuffer> membuf(new TMemoryBuffer());
  std::shared_ptr<TZlibTransport> zlib_trans(new TZlibTransport(membuf));
  zlib_trans->write(buf, buf_len);
  zlib_trans->finish();
  string tmp_buf;
  membuf->appendBufferToString(tmp_buf);
  // Modify a byte at the end of the buffer (part of the checksum).
  // On rare occasions, modifying a byte in the middle of the buffer
  // isn't caught by the checksum.
  //
  // (This happens especially often for the uniform buffer.  The
  // re-inflated data is correct, however.  I suspect in this case that
  // we're more likely to modify bytes that are part of zlib metadata
  // instead of the actual compressed data.)
  //
  // I've also seen some failure scenarios where a checksum failure isn't
  // reported, but zlib keeps trying to decode past the end of the data.
  // (When this occurs, verifyChecksum() throws an exception indicating
  // that the end of the data hasn't been reached.)  I haven't seen this
  // error when only modifying checksum bytes.
  int index = tmp_buf.size() - 1;
  tmp_buf[index]++;
  membuf->resetBuffer(const_cast<uint8_t*>(
                        reinterpret_cast<const uint8_t*>(tmp_buf.data())),
                      tmp_buf.length());

  boost::shared_array<uint8_t> mirror(new uint8_t[buf_len]);
  try {
    zlib_trans->readAll(mirror.get(), buf_len);
    zlib_trans->verifyChecksum();
    BOOST_ERROR("verifyChecksum() did not report an error");
  } catch (TZlibTransportException& ex) {
    BOOST_CHECK_EQUAL(ex.getType(), TTransportException::INTERNAL_ERROR);
  }
}

void test_write_after_flush(const uint8_t* buf, uint32_t buf_len) {
  // write some data
  std::shared_ptr<TMemoryBuffer> membuf(new TMemoryBuffer());
  std::shared_ptr<TZlibTransport> zlib_trans(new TZlibTransport(membuf));
  zlib_trans->write(buf, buf_len);

  // call finish()
  zlib_trans->finish();

  // make sure write() throws an error
  try {
    uint8_t write_buf[] = "a";
    zlib_trans->write(write_buf, 1);
    BOOST_ERROR("write() after finish() did not raise an exception");
  } catch (TTransportException& ex) {
    BOOST_CHECK_EQUAL(ex.getType(), TTransportException::BAD_ARGS);
  }

  // make sure flush() throws an error
  try {
    zlib_trans->flush();
    BOOST_ERROR("flush() after finish() did not raise an exception");
  } catch (TTransportException& ex) {
    BOOST_CHECK_EQUAL(ex.getType(), TTransportException::BAD_ARGS);
  }

  // make sure finish() throws an error
  try {
    zlib_trans->finish();
    BOOST_ERROR("finish() after finish() did not raise an exception");
  } catch (TTransportException& ex) {
    BOOST_CHECK_EQUAL(ex.getType(), TTransportException::BAD_ARGS);
  }
}

void test_no_write() {
  // Verify that no data is written to the underlying transport if we
  // never write data to the TZlibTransport.
  std::shared_ptr<TMemoryBuffer> membuf(new TMemoryBuffer());
  {
    // Create a TZlibTransport object, and immediately destroy it
    // when it goes out of scope.
    TZlibTransport w_zlib_trans(membuf);
  }

  BOOST_CHECK_EQUAL(membuf->available_read(), 0);
}

/*
 * Initialization
 */

#define ADD_TEST_CASE(suite, name, function, ...) \
  do { \
    ::std::ostringstream name_ss; \
    name_ss << name << "-" << BOOST_STRINGIZE(function); \
    ::boost::unit_test::test_case* tc = ::boost::unit_test::make_test_case( \
        ::std::bind(function, ## __VA_ARGS__), \
        name_ss.str()); \
    (suite)->add(tc); \
  } while (0)

void add_tests(unit_test::test_suite* suite,
               const uint8_t* buf,
               uint32_t buf_len,
               const char* name) {
  ADD_TEST_CASE(suite, name, test_write_then_read, buf, buf_len);
  ADD_TEST_CASE(suite, name, test_separate_checksum, buf, buf_len);
  ADD_TEST_CASE(suite, name, test_incomplete_checksum, buf, buf_len);
  ADD_TEST_CASE(suite, name, test_invalid_checksum, buf, buf_len);
  ADD_TEST_CASE(suite, name, test_write_after_flush, buf, buf_len);

  std::shared_ptr<SizeGenerator> size_32k(new ConstantSizeGenerator(1<<15));
  std::shared_ptr<SizeGenerator> size_lognormal(new LogNormalSizeGenerator(20, 30));
  ADD_TEST_CASE(suite, name << "-constant",
                test_read_write_mix, buf, buf_len,
                size_32k, size_32k);
  ADD_TEST_CASE(suite, name << "-lognormal-write",
                test_read_write_mix, buf, buf_len,
                size_lognormal, size_32k);
  ADD_TEST_CASE(suite, name << "-lognormal-read",
                test_read_write_mix, buf, buf_len,
                size_32k, size_lognormal);
  ADD_TEST_CASE(suite, name << "-lognormal-both",
                test_read_write_mix, buf, buf_len,
                size_lognormal, size_lognormal);

  // Test with a random size distribution,
  // but use the exact same distribution for reading as for writing.
  //
  // Because the SizeGenerator makes a copy of the random number generator,
  // both SizeGenerators should return the exact same set of values, since they
  // both start with random number generators in the same state.
  std::shared_ptr<SizeGenerator> write_size_gen(new LogNormalSizeGenerator(20, 30));
  std::shared_ptr<SizeGenerator> read_size_gen(new LogNormalSizeGenerator(20, 30));
  ADD_TEST_CASE(suite, name << "-lognormal-same-distribution",
                test_read_write_mix, buf, buf_len,
                write_size_gen, read_size_gen);
}

void print_usage(FILE* f, const char* argv0) {
  fprintf(f, "Usage: %s [boost_options] [options]\n", argv0);
  fprintf(f, "Options:\n");
  fprintf(f, "  --seed=<N>, -s <N>\n");
  fprintf(f, "  --help\n");
}

void parse_args(int argc, char* argv[]) {
  uint32_t seed = 0;
  bool has_seed = false;

  struct option long_opts[] = {
    { "help", false, nullptr, 'h' },
    { "seed", true, nullptr, 's' },
    { nullptr, 0, nullptr, 0 }
  };

  while (true) {
    optopt = 1;
    int optchar = getopt_long(argc, argv, "hs:", long_opts, nullptr);
    if (optchar == -1) {
      break;
    }

    switch (optchar) {
      case 's': {
        char *endptr;
        seed = strtol(optarg, &endptr, 0);
        if (endptr == optarg || *endptr != '\0') {
          fprintf(stderr, "invalid seed value \"%s\": must be a positive "
                  "integer\n", optarg);
          exit(1);
        }
        has_seed = true;
        break;
      }
      case 'h':
        print_usage(stdout, argv[0]);
        exit(0);
      case '?':
        exit(1);
      default:
        // Only happens if someone adds another option to the optarg string,
        // but doesn't update the switch statement to handle it.
        fprintf(stderr, "unknown option \"-%c\"\n", optchar);
        exit(1);
    }
  }

  if (!has_seed) {
    seed = time(nullptr);
  }

  printf("seed: %" PRIu32 "\n", seed);
  rng.seed(seed);
}

unit_test::test_suite* init_unit_test_suite(int argc, char* argv[]) {
  parse_args(argc, argv);

  unit_test::test_suite* suite =
    &boost::unit_test::framework::master_test_suite();
  suite->p_name.value = "ZlibTest";

  uint32_t buf_len = 1024*32;
  add_tests(suite, gen_uniform_buffer(buf_len, 'a'), buf_len, "uniform");
  add_tests(suite, gen_compressible_buffer(buf_len), buf_len, "compressible");
  add_tests(suite, gen_random_buffer(buf_len), buf_len, "random");

  suite->add(BOOST_TEST_CASE(test_no_write));

  return nullptr;
}

#include "skadi/sample.h"
#include "baldr/compression_utils.h"
#include "midgard/pointll.h"
#include "midgard/sequence.h"
#include "pixels.h"

#include <gtest/gtest.h>
#ifdef ENABLE_LZ4
#include <lz4frame.h>
#endif
#ifdef ENABLE_ZSTD
#include <zstd.h>
#endif

#include <cmath>
#include <filesystem>
#include <fstream>
#include <list>

using namespace valhalla;
using namespace valhalla::midgard;

namespace {

TEST(Sample, no_data) {
  // check the no data value
  skadi::sample s("test/data/this_is_not_a_directory");
  EXPECT_EQ(skadi::get_no_data_value(), -32768) << "No data value should be -32768";

  EXPECT_EQ(s.get(std::make_pair(0.0, 0.0)), skadi::get_no_data_value())
      << "Asked for point with no data should be no data value";

  EXPECT_EQ(s.get(std::make_pair(200.0, 200.0)), skadi::get_no_data_value())
      << "Asked for point outside of valid range";
}

TEST(Sample, create_tile) {
  // its annoying to have to get actual data but its also very boring to test with fake data
  // so we get some real data build the tests and then create the data on the fly
  // get the real tile and run the tests against it uncommenting the log infos in src/sample.cc
  // wget -q -O - http://s3.amazonaws.com/mapzen.valhalla/elevation/N40/N40W077.hgt.gz | gunzip >
  // test/data/N40W077.hgt hack the tests to run against that and turn that into something we can
  // build a tile out of grep -E '{[0-9,]+}' test/*.log | sed -e "s/.*{/{/g" | sort -n | tr '\n' ','
  // | sed -e "s/^/#include<cstdint>\n#include<unordered_map>\nstd::unordered_map<size_t,int16_t>
  // pixels {/g" -e "s/$/};/g" > test/pixels.h
  std::vector<int16_t> tile(3601 * 3601, 0);
  for (const auto& p : pixels) {
    tile[p.first] = p.second;
  }

  std::ofstream file("test/data/sample/N40/N40W077.hgt", std::ios::binary | std::ios::trunc);
  file.write(static_cast<const char*>(static_cast<void*>(tile.data())),
             sizeof(int16_t) * tile.size());

  // input for gzip
  auto src_func = [&tile](z_stream& s) -> int {
    s.next_in = static_cast<Byte*>(static_cast<void*>(tile.data()));
    s.avail_in = static_cast<unsigned int>(tile.size() * sizeof(int16_t));
    return Z_FINISH;
  };

  // output for gzip
  std::vector<char> dst_buffer(13000, 0);
  std::ofstream gzfile("test/data/samplegz/N40/N40W077.hgt.gz", std::ios::binary | std::ios::trunc);
  auto dst_func = [&dst_buffer, &gzfile](z_stream& s) -> void {
    // move these bytes to their final resting place
    auto chunk = s.total_out - gzfile.tellp();
    gzfile.write(static_cast<const char*>(static_cast<void*>(dst_buffer.data())), chunk);
    // if more input is coming
    if (s.avail_in > 0) {
      s.next_out = static_cast<Byte*>(static_cast<void*>(dst_buffer.data()));
      s.avail_out = dst_buffer.size();
    }
  };

  // gzip it
  EXPECT_TRUE(baldr::deflate(src_func, dst_func)) << "Can't write gzipped elevation tile";

#ifdef ENABLE_LZ4
  // lz4 it
  std::vector<char> lz4_buffer(tile.size() * sizeof(int16_t) * 2, 0);
  size_t out_bytes =
      LZ4F_compressFrame(lz4_buffer.data(), lz4_buffer.size(), static_cast<const void*>(tile.data()),
                         sizeof(int16_t) * tile.size(), NULL);
  EXPECT_TRUE(!LZ4F_isError(out_bytes) && out_bytes > 0) << "Can't write lz4 elevation tile";

  std::ofstream lzfile("test/data/samplelz4/N40/N40W077.hgt.lz4", std::ios::binary | std::ios::trunc);
  lzfile.write(static_cast<const char*>(static_cast<void*>(lz4_buffer.data())), out_bytes);
#endif

#ifdef ENABLE_ZSTD
  // each big-endian height minus the one to its left, as little-endian uint16s
  const auto* bytes = static_cast<const uint8_t*>(static_cast<void*>(tile.data()));
  std::vector<uint8_t> residuals(tile.size() * sizeof(int16_t));
  for (size_t row = 0; row < 3601; ++row) {
    uint16_t left = 0;
    for (size_t col = 0; col < 3601; ++col) {
      size_t i = 2 * (row * 3601 + col);
      uint16_t height = static_cast<uint16_t>((bytes[i] << 8) | bytes[i + 1]);
      uint16_t residual = static_cast<uint16_t>(height - left);
      residuals[i] = static_cast<uint8_t>(residual);
      residuals[i + 1] = static_cast<uint8_t>(residual >> 8);
      left = height;
    }
  }
  std::vector<char> zstd_buffer(ZSTD_compressBound(residuals.size()));
  size_t zstd_bytes =
      ZSTD_compress(zstd_buffer.data(), zstd_buffer.size(), residuals.data(), residuals.size(), 19);
  EXPECT_FALSE(ZSTD_isError(zstd_bytes)) << "Can't write zstd elevation tile";

  std::ofstream zstdfile("test/data/sampledzst/N40/N40W077.hgt.dzst",
                         std::ios::binary | std::ios::trunc);
  zstdfile.write(zstd_buffer.data(), zstd_bytes);
#endif
}

void _get(const std::string& location) {
  // check a single point
  skadi::sample s(location);
  EXPECT_NEAR(490, s.get(std::make_pair(-76.503915, 40.678783)), 1.0);

  // check a point near the edge of a tile
  EXPECT_NEAR(134, s.get(std::make_pair(-76.9, 40.0)), 1.0);

  // check a bunch
  std::list<std::pair<double, double>> postings = {
      {-76.537011, 40.723872}, // 300m
      {-76.537011, 40.726872}, // 350m
      {-76.537011, 40.729872}, // 450m
      {-76.537011, 40.732872}, // 325m
      {-76.537011, 40.735872}  // 250m
  };
  double riemann_sum = 0;
  auto heights = s.get_all(postings);
  for (const auto height : heights) {
    EXPECT_NE(height, skadi::get_no_data_value()) << "Should have heights for all of these points";
    riemann_sum += height;
  }
  EXPECT_NEAR(riemann_sum, 1675, 100) << "Area under discretized curve isn't right";
}

TEST(Sample, get) {
  _get("test/data/sample");
};

TEST(Sample, getgz) {
  _get("test/data/samplegz");
};

#ifdef ENABLE_LZ4
TEST(Sample, getlz4) {
  _get("test/data/samplelz4");
};
#endif

#ifdef ENABLE_ZSTD
TEST(Sample, getdzst) {
  _get("test/data/sampledzst");
};
#endif

struct testable_sample_t : public skadi::sample {
  testable_sample_t(const std::string& dir) : sample(dir) {
    {
      midgard::sequence<int16_t> s("test/data/blah.hgt", true);
      // first row is 1's second is 2's
      for (size_t i = 0; i < 3601 * 2; ++i)
        s.push_back(((i / 3601 + 1) & 0xFF) << 8);
      for (size_t i = 0; i < 3601 * (3601 - 2); ++i)
        s.push_back(((-32768 & 0xFF) << 8) | ((-32768 >> 8) & 0xFF));
    }
    add_single_tile("test/data/blah.hgt");
  }

  static uint16_t get_tile_index(const PointLL& coord) {
    return skadi::sample::get_tile_index(coord);
  }

  bool store(const std::string& path, const std::vector<char>& raw_data) {
    return skadi::sample::store(path, raw_data);
  }
};

TEST(Sample, edges) {
  testable_sample_t s("");

  // check 4 pixels
  auto n = .5f / 3600;
  float v = s.get(std::make_pair(-180.f + n, -89.f - n));

  EXPECT_NEAR(v, 1.5f, 0.1f);

  // check 2 pixels
  v = s.get(std::make_pair(-180.f + n, -89.f - n * 3));
  EXPECT_NEAR(v, 2.f, 0.1f);

  // check 0 pixels
  v = s.get(std::make_pair(-180.f + n, -89.f - n * 5));
  EXPECT_EQ(v, skadi::get_no_data_value()) << "Wrong value at location";
}

TEST(Sample, lazy_load) {
  // make sure there is no data there
  { std::ofstream file("test/data/sample/N00/N00E000.hgt", std::ios::binary | std::ios::trunc); }
  skadi::sample s("test/data/sample");
  EXPECT_EQ(s.get(std::make_pair(0.503915, 0.678783)), skadi::get_no_data_value())
      << "Asked for point with no data should be no data value";

  // put data there
  {
    std::vector<int16_t> tile(3601 * 3601, 0);
    for (const auto& p : pixels)
      tile[p.first] = p.second;
    std::ofstream file("test/data/sample/N00/N00E000.hgt", std::ios::binary | std::ios::trunc);
    file.write(static_cast<const char*>(static_cast<void*>(tile.data())),
               sizeof(int16_t) * tile.size());
  }

  // make sure there is now data there
  EXPECT_NEAR(490, s.get(std::make_pair(1 - 0.503915, 0.678783)), 1.0);
}

TEST(Sample, hgt_file_name) {
  // A mix of coordinates in each hemisphere and indices above and below 32767
  // (There was a bug where the 16-bit index was converted to a signed value and would be invalid)
  const auto long_island_index =
      testable_sample_t::get_tile_index(midgard::PointLL{-73.512143, 40.646556});
  ASSERT_EQ(valhalla::skadi::get_hgt_file_name(long_island_index), "/N40/N40W074.hgt");

  const auto equador_index =
      testable_sample_t::get_tile_index(midgard::PointLL{-78.504476, -0.212297});
  ASSERT_EQ(valhalla::skadi::get_hgt_file_name(equador_index), "/S01/S01W079.hgt");

  const auto brisbane_index =
      testable_sample_t::get_tile_index(midgard::PointLL{152.967888, -27.533658});
  ASSERT_EQ(valhalla::skadi::get_hgt_file_name(brisbane_index), "/S28/S28E152.hgt");

  const auto tokyo_index = testable_sample_t::get_tile_index(midgard::PointLL{139.737345, 35.628096});
  ASSERT_EQ(valhalla::skadi::get_hgt_file_name(tokyo_index), "/N35/N35E139.hgt");

  const auto amsterdam_index =
      testable_sample_t::get_tile_index(midgard::PointLL{4.898484, 52.380697});
  ASSERT_EQ(valhalla::skadi::get_hgt_file_name(amsterdam_index), "/N52/N52E004.hgt");
}

TEST(Sample, store) {
  // create tiles
  std::vector<int16_t> tile(3601 * 3601, 0);
  for (const auto& p : pixels) {
    tile[p.first] = p.second;
  }
  {
    std::ofstream file("test/data/sample/N00/N00E005.hgt", std::ios::binary | std::ios::trunc);
    file.write(static_cast<const char*>(static_cast<void*>(tile.data())),
               sizeof(int16_t) * tile.size());
  }

  {
    testable_sample_t s("test/data/sample");

    EXPECT_TRUE(s.store("/N00/N00E005.hgt", {}));
    EXPECT_TRUE(s.store("/N00/N00E005.hgt.gz", {}));

    // can be archived only with ".gz" format.
    EXPECT_FALSE(s.store("/N00/N00E005.hgt.tar", {}));

    // empty file
    EXPECT_FALSE(s.store("/N00/N00E009.hgt", {}));
  } // destroy s (drops mmaps) before removing the files on Windows

  std::error_code ec;
  std::filesystem::remove("test/data/sample/N00/N00E009.hgt", ec);
  std::filesystem::remove("test/data/sample/N00/N00E005.hgt", ec);
  std::filesystem::remove("test/data/sample/N00/N00E005.hgt.gz", ec);
}

} // namespace

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

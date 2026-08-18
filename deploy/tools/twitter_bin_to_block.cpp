// Convert a raw Twitter twemcache binary trace (packed 24-byte records from
// the OSDI'20 dataset: uint32 timestamp, uint64 obj_id, uint32 obj_size,
// int64 next_access_vtime) into the repository's block CSV format
// (version,depth,key).
//
// Usage: twitter_bin_to_block INPUT.bin OUTPUT.csv [MAX_REQUESTS]
//
// The output uses a monotonic logical version that increments whenever the
// raw timestamp changes, and depth=1 (flat object workload).
#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct Record {
  uint32_t timestamp;
  uint64_t obj_id;
  uint32_t obj_size;
  int64_t next_access_vtime;
} __attribute__((packed));

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::fprintf(stderr, "Usage: %s INPUT.bin OUTPUT.csv [MAX_REQUESTS]\n", argv[0]);
    return 2;
  }
  std::FILE* in = std::fopen(argv[1], "rb");
  std::FILE* out = std::fopen(argv[2], "w");
  if (!in || !out) {
    std::perror("twitter_bin_to_block");
    return 1;
  }
  const long long max_requests = argc == 4 ? std::atoll(argv[3]) : -1;

  std::fprintf(out, "version,depth,key\n");
  Record record{};
  int version = 0;
  uint32_t last_timestamp = 0;
  long long count = 0;
  bool first = true;
  while (std::fread(&record, sizeof(Record), 1, in) == 1) {
    if (max_requests >= 0 && count >= max_requests) break;
    if (first || record.timestamp != last_timestamp) {
      ++version;
      last_timestamp = record.timestamp;
      first = false;
    }
    std::fprintf(out, "%d,1,%llu\n", version,
                 static_cast<unsigned long long>(record.obj_id));
    ++count;
  }
  std::fprintf(stderr, "records=%lld versions=%d\n", count, version);
  std::fclose(in);
  std::fclose(out);
  return 0;
}

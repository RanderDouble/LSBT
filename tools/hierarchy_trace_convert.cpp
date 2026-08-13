#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {
constexpr int kPathDepth = 20;
constexpr int kNodeBytes = 64;

struct NodeKey {
  std::array<uint8_t, kPathDepth> bytes{};
  uint8_t depth = 0;
  bool operator==(const NodeKey& other) const {
    return depth == other.depth && bytes == other.bytes;
  }
};

struct NodeKeyHash {
  size_t operator()(const NodeKey& key) const {
    size_t value = key.depth;
    for (int index = 0; index < key.depth; ++index) value = value * 131 + key.bytes[index];
    return value;
  }
};

int Hex(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

uint64_t Intern(const NodeKey& node,
                std::unordered_map<NodeKey, uint64_t, NodeKeyHash>* ids) {
  auto found = ids->find(node);
  if (found != ids->end()) return found->second;
  const uint64_t id = ids->size() + 1;
  ids->emplace(node, id);
  return id;
}

void Emit(std::ofstream* output, uint64_t time, uint64_t id, int depth) {
  *output << time << ',' << id << ',' << kNodeBytes << ',' << depth << '\n';
}

void EmitAddress(std::string_view address, uint64_t time, std::ofstream* output,
                 std::unordered_map<NodeKey, uint64_t, NodeKeyHash>* ids) {
  if (address.size() != 42 || address.substr(0, 2) != "0x") {
    throw std::runtime_error("invalid Ethereum address");
  }
  NodeKey node;
  for (int index = 0; index < kPathDepth; ++index) {
    const int high = Hex(address[2 + index * 2]);
    const int low = Hex(address[3 + index * 2]);
    if (high < 0 || low < 0) throw std::runtime_error("invalid hexadecimal address");
    node.bytes[index] = static_cast<uint8_t>(high * 16 + low);
  }
  for (int depth = kPathDepth; depth >= 1; --depth) {
    node.depth = static_cast<uint8_t>(depth);
    Emit(output, time, Intern(node, ids), depth);
    node.bytes[depth - 1] = 0;
  }
}

void ConvertRaw(const std::string& input_path, const std::string& output_path) {
  std::ifstream input(input_path);
  std::ofstream output(output_path);
  if (!input || !output) throw std::runtime_error("cannot open input or output");
  output << "time,obj_id,obj_size,depth\n";
  std::unordered_map<NodeKey, uint64_t, NodeKeyHash> ids;
  std::string line;
  uint64_t transaction = 0;
  std::getline(input, line);
  while (std::getline(input, line)) {
    const size_t first = line.find(',');
    const size_t second = line.find(',', first + 1);
    if (first == std::string::npos || second == std::string::npos) continue;
    ++transaction;
    EmitAddress(std::string_view(line).substr(first + 1, second - first - 1), transaction,
                &output, &ids);
    std::string_view to(line.data() + second + 1, line.size() - second - 1);
    if (to != "\"\"") EmitAddress(to, transaction, &output, &ids);
  }
  std::cerr << "unique_nodes=" << ids.size() << " node_bytes=" << kNodeBytes << '\n';
}

void ConvertLsbt(const std::string& input_path, const std::string& output_path) {
  std::ifstream input(input_path);
  std::ofstream output(output_path);
  if (!input || !output) throw std::runtime_error("cannot open input or output");
  output << "time,obj_id,obj_size,depth\n";
  std::unordered_map<std::string, uint64_t> ids;
  std::string line;
  std::getline(input, line);
  while (std::getline(input, line)) {
    const size_t first = line.find(',');
    const size_t second = line.find(',', first + 1);
    if (first == std::string::npos || second == std::string::npos) continue;
    const uint64_t block = std::stoull(line.substr(0, first));
    const int depth = std::stoi(line.substr(first + 1, second - first - 1));
    const std::string key = line.substr(second + 1);
    auto [found, inserted] = ids.emplace(key, ids.size() + 1);
    Emit(&output, block, found->second, depth);
  }
  std::cerr << "unique_nodes=" << ids.size() << " node_bytes=" << kNodeBytes << '\n';
}

void ConvertTwitter(const std::string& input_path, const std::string& output_path) {
  std::ifstream input(input_path);
  std::ofstream output(output_path);
  if (!input || !output) throw std::runtime_error("cannot open input or output");
  output << "time,obj_id,obj_size,depth\n";
  std::unordered_map<std::string, uint64_t> ids;
  std::string line;
  std::getline(input, line);
  while (std::getline(input, line)) {
    const size_t first = line.find(',');
    const size_t second = line.find(',', first + 1);
    const size_t third = line.find(',', second + 1);
    if (first == std::string::npos || second == std::string::npos || third == std::string::npos) continue;
    const uint64_t time = std::stoull(line.substr(0, first));
    const std::string key = line.substr(first + 1, second - first - 1);
    auto [found, inserted] = ids.emplace(key, ids.size() + 1);
    Emit(&output, time, found->second, 1);
  }
  std::cerr << "unique_nodes=" << ids.size() << " node_bytes=" << kNodeBytes << '\n';
}

void ConvertCloudPhysicsIO(const std::string& input_path, const std::string& output_path) {
  std::ifstream input(input_path);
  std::ofstream output(output_path);
  if (!input || !output) throw std::runtime_error("cannot open input or output");
  output << "time,obj_id,obj_size,depth\n";
  std::unordered_map<std::string, uint64_t> ids;
  std::string line;
  std::getline(input, line);
  while (std::getline(input, line)) {
    const size_t first = line.find(',');
    const size_t second = line.find(',', first + 1);
    const size_t third = line.find(',', second + 1);
    const size_t fourth = line.find(',', third + 1);
    if (first == std::string::npos || second == std::string::npos ||
        third == std::string::npos || fourth == std::string::npos) continue;
    const uint64_t time = std::stoull(line.substr(first + 1, second - first - 1));
    const std::string key = line.substr(fourth + 1);
    auto [found, inserted] = ids.emplace(key, ids.size() + 1);
    Emit(&output, time, found->second, 1);
  }
  std::cerr << "unique_nodes=" << ids.size() << " node_bytes=" << kNodeBytes << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4 || (std::string(argv[1]) != "raw" && std::string(argv[1]) != "lsbt" &&
                    std::string(argv[1]) != "twitter" && std::string(argv[1]) != "cloudphysics")) {
    std::cerr << "Usage: hierarchy_trace_convert raw|lsbt|twitter|cloudphysics INPUT.csv OUTPUT.csv\n";
    return 2;
  }
  try {
    if (std::string(argv[1]) == "raw") ConvertRaw(argv[2], argv[3]);
    else if (std::string(argv[1]) == "lsbt") ConvertLsbt(argv[2], argv[3]);
    else if (std::string(argv[1]) == "twitter") ConvertTwitter(argv[2], argv[3]);
    else ConvertCloudPhysicsIO(argv[2], argv[3]);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}

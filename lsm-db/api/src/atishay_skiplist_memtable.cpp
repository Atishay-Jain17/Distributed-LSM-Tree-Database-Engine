#include "lsmdb/api/atishay_skiplist_memtable.h"

#include <limits>
#include <mutex>
#include <stdexcept>

namespace lsmdb::api {
namespace {
std::string EncodeInvertedSequence(SequenceNumber seq) {
  std::string out;
  out.reserve(8);
  const SequenceNumber inverted = ~seq;
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back(static_cast<char>((inverted >> shift) & 0xff));
  return out;
}
}  // namespace

std::string AtishaySkipListMemTable::KeyPrefix(const std::string& key) {
  std::string prefix;
  prefix.reserve(key.size() + 2);
  for (unsigned char byte : key) {
    if (byte == 0) {
      prefix.push_back('\0');
      prefix.push_back(static_cast<char>(0xff));
    } else {
      prefix.push_back(static_cast<char>(byte));
    }
  }
  prefix.append("\0\0", 2);  // escaped terminator; preserves prefix ordering for arbitrary binary keys
  return prefix;
}

std::string AtishaySkipListMemTable::VersionKey(const std::string& key, SequenceNumber seq) {
  return KeyPrefix(key) + EncodeInvertedSequence(seq);
}

std::string AtishaySkipListMemTable::DecodeUserKey(const std::string& composite,
                                                   std::size_t& suffix_offset) {
  std::string key;
  std::size_t i = 0;
  while (i < composite.size()) {
    const unsigned char byte = static_cast<unsigned char>(composite[i++]);
    if (byte != 0) {
      key.push_back(static_cast<char>(byte));
      continue;
    }
    if (i >= composite.size()) throw std::logic_error("malformed SkipList MemTable key");
    const unsigned char escaped = static_cast<unsigned char>(composite[i++]);
    if (escaped == 0xff) {
      key.push_back('\0');
      continue;
    }
    if (escaped == 0) {
      suffix_offset = i;
      if (composite.size() - suffix_offset != 8)
        throw std::logic_error("malformed SkipList MemTable sequence suffix");
      return key;
    }
    throw std::logic_error("malformed SkipList MemTable key escape");
  }
  throw std::logic_error("unterminated SkipList MemTable key");
}

SequenceNumber AtishaySkipListMemTable::DecodeSequence(const std::string& composite,
                                                       std::size_t suffix_offset) {
  SequenceNumber inverted = 0;
  for (std::size_t i = suffix_offset; i < suffix_offset + 8; ++i)
    inverted = (inverted << 8) | static_cast<unsigned char>(composite[i]);
  return ~inverted;
}

void AtishaySkipListMemTable::Add(SequenceNumber seq, ValueType type, const std::string& key,
                                  const std::string& value) {
  if (seq == 0) throw std::invalid_argument("MemTable sequence number must be positive");
  if (type != ValueType::kValue && type != ValueType::kTombstone)
    throw std::invalid_argument("unknown MemTable value type");
  if (type == ValueType::kTombstone && !value.empty())
    throw std::invalid_argument("tombstones cannot carry a value");

  std::unique_lock<std::shared_mutex> lock(mu_);
  const std::string prefix = KeyPrefix(key);
  std::string newest_target = prefix + EncodeInvertedSequence(std::numeric_limits<SequenceNumber>::max());
  auto newest = entries_.Seek(newest_target);
  if (newest.Valid() && newest.Key().compare(0, prefix.size(), prefix) == 0) {
    std::size_t offset = 0;
    (void)DecodeUserKey(newest.Key(), offset);
    if (seq <= DecodeSequence(newest.Key(), offset))
      throw std::logic_error("MemTable sequence numbers must increase for each key");
  }

  std::string stored_value;
  stored_value.reserve(value.size() + 1);
  stored_value.push_back(static_cast<char>(type));
  if (type == ValueType::kValue) stored_value += value;
  entries_.Insert(VersionKey(key, seq), stored_value);
}

LookupResult AtishaySkipListMemTable::Get(const std::string& key, SequenceNumber read_seq) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  const std::string prefix = KeyPrefix(key);
  auto entry = entries_.Seek(prefix + EncodeInvertedSequence(read_seq));
  if (!entry.Valid() || entry.Key().compare(0, prefix.size(), prefix) != 0) return {};

  std::size_t offset = 0;
  (void)DecodeUserKey(entry.Key(), offset);
  LookupResult result;
  result.sequence = DecodeSequence(entry.Key(), offset);
  if (result.sequence > read_seq) throw std::logic_error("SkipList version search returned a future value");
  if (entry.Value().empty()) throw std::logic_error("malformed SkipList MemTable value");
  const auto type = static_cast<ValueType>(static_cast<unsigned char>(entry.Value()[0]));
  if (type == ValueType::kTombstone) {
    result.state = LookupState::kDeleted;
  } else if (type == ValueType::kValue) {
    result.state = LookupState::kFound;
    result.value.assign(entry.Value().data() + 1, entry.Value().size() - 1);
  } else {
    throw std::logic_error("unknown SkipList MemTable value type");
  }
  return result;
}

std::size_t AtishaySkipListMemTable::ApproximateMemoryUsage() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return entries_.ApproximateBytes();
}

std::size_t AtishaySkipListMemTable::EntryCount() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return entries_.EntryCount();
}

void AtishaySkipListMemTable::ForEach(const EntryVisitor& visit) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto entry = entries_.Begin();
  while (entry.Valid()) {
    std::size_t offset = 0;
    const std::string key = DecodeUserKey(entry.Key(), offset);
    const SequenceNumber seq = DecodeSequence(entry.Key(), offset);
    if (entry.Value().empty()) throw std::logic_error("malformed SkipList MemTable value");
    const auto type = static_cast<ValueType>(static_cast<unsigned char>(entry.Value()[0]));
    if (type != ValueType::kValue && type != ValueType::kTombstone)
      throw std::logic_error("unknown SkipList MemTable value type");
    const std::string value = type == ValueType::kValue ? entry.Value().substr(1) : std::string();
    visit(seq, type, key, value);
    entry.Next();
  }
}

}  // namespace lsmdb::api

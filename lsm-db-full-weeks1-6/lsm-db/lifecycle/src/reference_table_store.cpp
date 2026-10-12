#include "lsmdb/lifecycle/reference_table_store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <tuple>

namespace lsmdb::lifecycle {

namespace fs = std::filesystem;
using api::LookupState;
using api::ValueType;

namespace {
// Record: u32 key_len | u32 value_len | u64 seq | u8 type | key | value   (host byte order)
struct Header {
  std::uint32_t klen;
  std::uint32_t vlen;
  std::uint64_t seq;
  std::uint8_t type;
};
using Entry = std::tuple<std::string, SequenceNumber, ValueType, std::string>;
}  // namespace

// One immutable file. The destructor deletes it if the table was replaced by a compaction, so a
// file lives exactly as long as some reader (or the live list) still references the table.
class ReferenceTableStore::Table {
 public:
  explicit Table(fs::path p) : path(std::move(p)) {}
  ~Table() {
    if (obsolete.load()) {
      std::error_code ec;
      fs::remove(path, ec);
    }
  }
  fs::path path;
  std::atomic<bool> obsolete{false};
};

ReferenceTableStore::ReferenceTableStore(fs::path dir, ReferenceTableStoreOptions options)
    : dir_(std::move(dir)), opt_(options), tables_(std::make_shared<TableList>()) {
  fs::create_directories(dir_);
  for (const auto& e : fs::directory_iterator(dir_)) {      // leftovers of a crashed write
    if (e.path().extension() == ".tmp") fs::remove(e.path());
  }
}

ReferenceTableStore::~ReferenceTableStore() = default;

std::shared_ptr<const ReferenceTableStore::TableList> ReferenceTableStore::Current() const {
  std::lock_guard<std::mutex> l(mu_);
  return tables_;
}

std::shared_ptr<ReferenceTableStore::Table> ReferenceTableStore::WriteFile(
    const std::vector<Entry>& entries) {
  char name[32];
  std::snprintf(name, sizeof name, "%06llu",
                static_cast<unsigned long long>(next_file_.fetch_add(1)));
  const fs::path tmp = dir_ / (std::string(name) + ".tmp");
  const fs::path fin = dir_ / (std::string(name) + ".sst");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create " + tmp.string());
    for (const auto& [k, seq, type, v] : entries) {
      Header h{static_cast<std::uint32_t>(k.size()), static_cast<std::uint32_t>(v.size()), seq,
               static_cast<std::uint8_t>(type)};
      out.write(reinterpret_cast<const char*>(&h.klen), 4);
      out.write(reinterpret_cast<const char*>(&h.vlen), 4);
      out.write(reinterpret_cast<const char*>(&h.seq), 8);
      out.write(reinterpret_cast<const char*>(&h.type), 1);
      out.write(k.data(), static_cast<std::streamsize>(k.size()));
      out.write(v.data(), static_cast<std::streamsize>(v.size()));
    }
    out.flush();
    if (!out) {
      out.close();
      std::error_code ec;
      fs::remove(tmp, ec);
      throw std::runtime_error("write failed for " + tmp.string());
    }
  }
  fs::rename(tmp, fin);   // atomic: a .sst file is always complete
  return std::make_shared<Table>(fin);
}

void ReferenceTableStore::AddTable(const MemTable& imm) {
  std::vector<Entry> entries;
  imm.ForEach([&](SequenceNumber seq, ValueType type, const std::string& k, const std::string& v) {
    entries.emplace_back(k, seq, type, v);
  });
  if (entries.empty()) return;
  auto table = WriteFile(entries);
  std::lock_guard<std::mutex> l(mu_);
  auto next = std::make_shared<TableList>();
  next->push_back(table);                                   // newest first
  next->insert(next->end(), tables_->begin(), tables_->end());
  tables_ = std::move(next);
}

LookupResult ReferenceTableStore::Get(const std::string& key, SequenceNumber read_seq) const {
  auto list = Current();   // reader keeps these tables (and files) alive until it returns
  for (const auto& t : *list) {
    std::ifstream in(t->path, std::ios::binary);
    if (!in) throw std::runtime_error("table file missing: " + t->path.string());
    for (;;) {
      Header h{};
      if (!in.read(reinterpret_cast<char*>(&h.klen), 4)) break;
      in.read(reinterpret_cast<char*>(&h.vlen), 4);
      in.read(reinterpret_cast<char*>(&h.seq), 8);
      in.read(reinterpret_cast<char*>(&h.type), 1);
      std::string k(h.klen, '\0');
      in.read(k.data(), h.klen);
      if (!in) throw std::runtime_error("corrupt table file: " + t->path.string());
      if (k > key) break;                                    // sorted: key is not in this table
      if (k < key || h.seq > read_seq) {
        in.seekg(h.vlen, std::ios::cur);
        continue;
      }
      LookupResult r;
      r.sequence = h.seq;
      if (static_cast<ValueType>(h.type) == ValueType::kTombstone) {
        r.state = LookupState::kDeleted;
        return r;
      }
      r.state = LookupState::kFound;
      r.value.resize(h.vlen);
      in.read(r.value.data(), h.vlen);
      return r;
    }
  }
  return LookupResult{};
}

bool ReferenceTableStore::NeedsCompaction() const {
  return Current()->size() >= opt_.compaction_trigger;
}

void ReferenceTableStore::Compact(SequenceNumber oldest_snapshot) {
  std::lock_guard<std::mutex> one(compact_mu_);
  auto inputs = Current();                 // a flush may add newer tables while we merge
  if (inputs->size() < 2) return;

  std::vector<Entry> all;
  for (const auto& t : *inputs) {
    std::ifstream in(t->path, std::ios::binary);
    if (!in) throw std::runtime_error("table file missing: " + t->path.string());
    for (;;) {
      Header h{};
      if (!in.read(reinterpret_cast<char*>(&h.klen), 4)) break;
      in.read(reinterpret_cast<char*>(&h.vlen), 4);
      in.read(reinterpret_cast<char*>(&h.seq), 8);
      in.read(reinterpret_cast<char*>(&h.type), 1);
      std::string k(h.klen, '\0'), v(h.vlen, '\0');
      in.read(k.data(), h.klen);
      in.read(v.data(), h.vlen);
      if (!in) throw std::runtime_error("corrupt table file: " + t->path.string());
      all.emplace_back(std::move(k), h.seq, static_cast<ValueType>(h.type), std::move(v));
    }
  }
  std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) {
    if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
    return std::get<1>(a) > std::get<1>(b);                  // newest first within a key
  });

  // Keep every version newer than the oldest snapshot, plus the newest one at or below it.
  // The inputs are the OLDEST tables, so a tombstone at that floor has nothing below it to hide
  // and can be dropped together with everything older.
  std::vector<Entry> out;
  std::string cur;
  bool have_cur = false, floor_seen = false;
  for (auto& e : all) {
    if (!have_cur || cur != std::get<0>(e)) { cur = std::get<0>(e); have_cur = true; floor_seen = false; }
    if (std::get<1>(e) > oldest_snapshot) { out.push_back(std::move(e)); continue; }
    if (floor_seen) continue;
    floor_seen = true;
    if (std::get<2>(e) == ValueType::kValue) out.push_back(std::move(e));
  }

  std::shared_ptr<Table> output;
  if (!out.empty()) output = WriteFile(out);

  std::lock_guard<std::mutex> l(mu_);
  auto next = std::make_shared<TableList>();
  for (const auto& t : *tables_) {
    const bool is_input = std::find(inputs->begin(), inputs->end(), t) != inputs->end();
    if (!is_input) next->push_back(t);                       // tables flushed meanwhile stay newer
  }
  if (output) next->push_back(output);                       // oldest position
  for (const auto& t : *inputs) t->obsolete.store(true);     // file removed by the last reader
  tables_ = std::move(next);
}

std::size_t ReferenceTableStore::TableCount() const { return Current()->size(); }

std::vector<fs::path> ReferenceTableStore::LivePaths() const {
  std::vector<fs::path> v;
  for (const auto& t : *Current()) v.push_back(t->path);
  return v;
}

std::shared_ptr<const void> ReferenceTableStore::Pin() const {
  return std::static_pointer_cast<const void>(Current());
}

}  // namespace lsmdb::lifecycle

#include "lsmdb/lifecycle/reference_table_store.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

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

void PublishDurably(const fs::path& temporary, const fs::path& target) {
#ifdef _WIN32
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot reopen for sync: " + temporary.string());
  const BOOL synced = FlushFileBuffers(file);
  const BOOL closed = CloseHandle(file);
  if (!synced) throw std::runtime_error("cannot sync: " + temporary.string());
  if (!closed) throw std::runtime_error("cannot close after sync: " + temporary.string());
  if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("cannot publish: " + target.string());
#else
  const int file = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
  if (file < 0) throw std::runtime_error("cannot reopen for sync: " + temporary.string());
  const int file_sync = ::fsync(file);
  const int file_error = errno;
  const int file_close = ::close(file);
  if (file_sync != 0) {
    errno = file_error;
    throw std::runtime_error("cannot sync: " + temporary.string() + ": " + std::strerror(errno));
  }
  if (file_close != 0) throw std::runtime_error("cannot close after sync: " + temporary.string());
  if (::rename(temporary.c_str(), target.c_str()) != 0)
    throw std::runtime_error("cannot publish: " + target.string() + ": " + std::strerror(errno));
  fs::path parent = target.parent_path();
  if (parent.empty()) parent = ".";
  const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) throw std::runtime_error("cannot open directory for sync: " + parent.string());
  const int directory_sync = ::fsync(directory);
  const int directory_error = errno;
  const int directory_close = ::close(directory);
  if (directory_sync != 0) {
    errno = directory_error;
    throw std::runtime_error("cannot sync directory: " + parent.string() + ": " + std::strerror(errno));
  }
  if (directory_close != 0) throw std::runtime_error("cannot close directory after sync: " + parent.string());
#endif
}

void WriteAndPublish(const fs::path& temporary, const fs::path& target, const std::string& contents) {
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create: " + temporary.string());
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    if (!out) throw std::runtime_error("cannot write: " + temporary.string());
  }
  PublishDurably(temporary, target);
}
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
  std::map<std::uint64_t, fs::path> discovered;
  std::uint64_t next = 1;
  SequenceNumber highest = 0;
  for (const auto& e : fs::directory_iterator(dir_)) {
    if (e.path().extension() == ".tmp") { fs::remove(e.path()); continue; }
    if (e.path().extension() != ".sst") continue;
    const std::string stem = e.path().stem().string();
    std::uint64_t id = 0;
    try { std::size_t used = 0; id = std::stoull(stem, &used); if (used != stem.size()) continue; }
    catch (...) { continue; }
    next = std::max(next, id + 1);
    if (!discovered.emplace(id, e.path()).second)
      throw std::runtime_error("duplicate table file number in " + dir_.string());
  }

  std::vector<std::pair<std::uint64_t, fs::path>> selected;
  const fs::path live_manifest = dir_ / "tables.manifest";
  if (fs::exists(live_manifest)) {
    std::ifstream manifest(live_manifest);
    if (!manifest) throw std::runtime_error("cannot read table manifest: " + live_manifest.string());
    std::string filename;
    std::set<std::uint64_t> seen;
    while (std::getline(manifest, filename)) {
      if (manifest.eof()) throw std::runtime_error("truncated table manifest: " + live_manifest.string());
      const fs::path name(filename);
      std::uint64_t id = 0;
      try { std::size_t used = 0; id = std::stoull(name.stem().string(), &used); if (used != name.stem().string().size()) throw std::invalid_argument("bad id"); }
      catch (...) { throw std::runtime_error("corrupt table manifest: " + live_manifest.string()); }
      if (name.filename() != name || name.extension() != ".sst" || !seen.insert(id).second)
        throw std::runtime_error("corrupt table manifest: " + live_manifest.string());
      auto it = discovered.find(id);
      if (it == discovered.end() || it->second.filename() != name.filename())
        throw std::runtime_error("manifest table is missing: " + filename);
      selected.emplace_back(id, it->second);
    }
  } else {
    for (const auto& item : discovered) selected.push_back(item);
    std::sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  }

  std::vector<std::pair<std::uint64_t, std::shared_ptr<Table>>> found;
  for (const auto& [id, table_path] : selected) {
    const auto& path = table_path;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open table file: " + path.string());
    for (;;) {
      Header h{};
      if (!in.read(reinterpret_cast<char*>(&h.klen), 4)) {
        if (in.gcount() != 0 || !in.eof()) throw std::runtime_error("corrupt table file: " + path.string());
        break;
      }
      in.read(reinterpret_cast<char*>(&h.vlen), 4);
      in.read(reinterpret_cast<char*>(&h.seq), 8);
      in.read(reinterpret_cast<char*>(&h.type), 1);
      if (!in || h.klen == 0 || h.klen > (1u << 20) || h.vlen > (1u << 24) ||
          (h.type != static_cast<std::uint8_t>(ValueType::kValue) &&
           h.type != static_cast<std::uint8_t>(ValueType::kTombstone)))
        throw std::runtime_error("corrupt table file: " + path.string());
      in.seekg(static_cast<std::streamoff>(h.klen) + h.vlen, std::ios::cur);
      if (!in) throw std::runtime_error("truncated table file: " + path.string());
      highest = std::max(highest, h.seq);
    }
    found.emplace_back(id, std::make_shared<Table>(path));
  }
  const fs::path sequence_file = dir_ / "sequence.manifest";
  if (fs::exists(sequence_file)) {
    std::ifstream manifest(sequence_file);
    if (!manifest) throw std::runtime_error("cannot read sequence manifest: " + sequence_file.string());
    std::string line;
    while (std::getline(manifest, line)) {
      // A partial final line can result from a crash while appending the checkpoint.
      if (manifest.eof()) break;
      try {
        std::size_t used = 0;
        const auto seq = std::stoull(line, &used);
        if (used != line.size()) throw std::invalid_argument("trailing data");
        highest = std::max(highest, static_cast<SequenceNumber>(seq));
      } catch (...) {
        throw std::runtime_error("corrupt sequence manifest: " + sequence_file.string());
      }
    }
  }
  if (fs::exists(live_manifest)) {
    std::set<std::uint64_t> active;
    for (const auto& item : found) active.insert(item.first);
    for (const auto& [id, path] : discovered) {
      if (!active.count(id)) { std::error_code ec; fs::remove(path, ec); }
    }
  }
  // A table can be the only surviving record of the sequence high-water mark
  // (for example after an older build or a crash between table and checkpoint
  // publication). Re-persist it before compaction can discard that table.
  PersistSequence(highest);
  auto loaded = std::make_shared<TableList>();
  for (auto& entry : found) loaded->push_back(std::move(entry.second));
  tables_ = std::move(loaded);
  next_file_.store(next);
  last_sequence_.store(highest);
}

ReferenceTableStore::~ReferenceTableStore() = default;

std::shared_ptr<const ReferenceTableStore::TableList> ReferenceTableStore::Current() const {
  std::lock_guard<std::mutex> l(mu_);
  return tables_;
}

void ReferenceTableStore::PersistManifest(const TableList& tables) const {
  const fs::path temp = dir_ / "tables.manifest.tmp";
  const fs::path target = dir_ / "tables.manifest";
  std::string contents;
  for (const auto& table : tables) contents += table->path.filename().string() + '\n';
  WriteAndPublish(temp, target, contents);
}

void ReferenceTableStore::PersistSequence(SequenceNumber sequence) const {
  WriteAndPublish(dir_ / "sequence.manifest.tmp", dir_ / "sequence.manifest",
                  std::to_string(sequence) + "\n");
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
  PublishDurably(tmp, fin);   // table content and directory entry are durable before manifest publish
  return std::make_shared<Table>(fin);
}

void ReferenceTableStore::AddTable(const MemTable& imm) {
  std::vector<Entry> entries;
  SequenceNumber highest = 0;
  imm.ForEach([&](SequenceNumber seq, ValueType type, const std::string& k, const std::string& v) {
    entries.emplace_back(k, seq, type, v);
    highest = std::max(highest, seq);
  });
  if (entries.empty()) return;
  auto table = WriteFile(entries);
  std::lock_guard<std::mutex> l(mu_);
  auto next = std::make_shared<TableList>();
  next->push_back(table);                                   // newest first
  next->insert(next->end(), tables_->begin(), tables_->end());
  PersistManifest(*next);
  if (highest > last_sequence_.load()) PersistSequence(highest);
  last_sequence_.store(std::max(last_sequence_.load(), highest));
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
  PersistManifest(*next);
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

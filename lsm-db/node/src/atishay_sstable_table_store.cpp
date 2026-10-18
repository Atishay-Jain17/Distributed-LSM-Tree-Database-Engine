#include "lsmdb/node/atishay_sstable_table_store.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <cerrno>
#include <cstring>
#include <tuple>

#include <fcntl.h>
#include <unistd.h>

#include "lsmdb/storage/sstable_writer.h"

namespace lsmdb::node {
namespace fs = std::filesystem;
using lifecycle::LookupResult;
using lifecycle::SequenceNumber;
using api::LookupState;
using api::ValueType;

std::uint64_t ParseTableId(const std::string& text) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    throw std::invalid_argument("table id must be a positive decimal number");
  std::size_t used = 0;
  const auto id = std::stoull(text, &used);
  if (used != text.size() || id == 0 || id == std::numeric_limits<std::uint64_t>::max())
    throw std::invalid_argument("table id is outside the supported range");
  return id;
}

void DurableReplace(const fs::path& temporary, const fs::path& target, const std::string& bytes) {
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) throw std::runtime_error("cannot create " + temporary.string() + ": " + std::strerror(errno));
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      const std::string message = std::strerror(errno);
      ::close(fd);
      throw std::runtime_error("cannot write " + temporary.string() + ": " + message);
    }
    written += static_cast<std::size_t>(count);
  }
  if (::fsync(fd) != 0) {
    const std::string message = std::strerror(errno);
    ::close(fd);
    throw std::runtime_error("cannot sync " + temporary.string() + ": " + message);
  }
  if (::close(fd) != 0) throw std::runtime_error("cannot close " + temporary.string());
  if (::rename(temporary.c_str(), target.c_str()) != 0)
    throw std::runtime_error("cannot publish " + target.string() + ": " + std::strerror(errno));
  const fs::path parent = target.parent_path().empty() ? fs::path(".") : target.parent_path();
  const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) throw std::runtime_error("cannot open directory for sync " + parent.string());
  const int sync_result = ::fsync(directory);
  ::close(directory);
  if (sync_result != 0) throw std::runtime_error("cannot sync directory " + parent.string());
}

class AtishaySSTableTableStore::Table {
 public:
  explicit Table(fs::path p) : path(std::move(p)), reader(storage::SSTableReader::Open(path.string())) {}
  ~Table() {
    if (obsolete.load()) {
      std::error_code ec;
      fs::remove(path, ec);
    }
  }
  fs::path path;
  std::unique_ptr<storage::SSTableReader> reader;
  std::atomic<bool> obsolete{false};
};

AtishaySSTableTableStore::AtishaySSTableTableStore(fs::path dir, std::size_t compaction_trigger)
    : dir_(std::move(dir)), compaction_trigger_(compaction_trigger),
      tables_(std::make_shared<TableList>()) {
  if (compaction_trigger_ < 2) throw std::invalid_argument("compaction trigger must be at least two");
  fs::create_directories(dir_);
  std::map<std::uint64_t, fs::path> discovered;
  std::uint64_t next_id = 1;
  SequenceNumber highest = 0;
  for (const auto& item : fs::directory_iterator(dir_)) {
    if (item.path().extension() == ".tmp") { fs::remove(item.path()); continue; }
    if (item.path().extension() != ".sst") continue;
    const auto stem = item.path().stem().string();
    std::uint64_t id;
    try { id = ParseTableId(stem); }
    catch (...) { continue; }
    if (!discovered.emplace(id, item.path()).second)
      throw std::runtime_error("duplicate SSTable number in " + dir_.string());
    next_id = std::max(next_id, id + 1);
  }

  std::vector<std::pair<std::uint64_t, fs::path>> selected;
  const fs::path manifest = dir_ / "tables.manifest";
  if (fs::exists(manifest)) {
    std::ifstream in(manifest);
    if (!in) throw std::runtime_error("cannot read SSTable manifest");
    std::set<std::uint64_t> seen;
    std::string filename;
    while (std::getline(in, filename)) {
      const fs::path name(filename);
      std::uint64_t id;
      try { id = ParseTableId(name.stem().string()); }
      catch (...) { throw std::runtime_error("corrupt SSTable manifest"); }
      if (name.filename() != name || name.extension() != ".sst" ||
          !seen.insert(id).second)
        throw std::runtime_error("corrupt SSTable manifest");
      auto found = discovered.find(id);
      if (found == discovered.end() || found->second.filename() != name.filename())
        throw std::runtime_error("manifest references a missing SSTable: " + filename);
      selected.emplace_back(id, found->second);
    }
    if (!in.eof()) throw std::runtime_error("error reading SSTable manifest");
  } else {
    for (const auto& entry : discovered) selected.push_back(entry);
    std::sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  }

  auto loaded = std::make_shared<TableList>();
  for (const auto& [id, path] : selected) {
    (void)id;
    auto table = std::make_shared<Table>(path);
    table->reader->VerifyChecksum();
    highest = std::max(highest, table->reader->metadata().max_seq);
    loaded->push_back(std::move(table));
  }
  const fs::path sequence_file = dir_ / "sequence.manifest";
  if (fs::exists(sequence_file)) {
    std::ifstream in(sequence_file);
    SequenceNumber recorded = 0;
    if (!(in >> recorded)) throw std::runtime_error("corrupt sequence manifest");
    in >> std::ws;
    if (!in.eof()) throw std::runtime_error("corrupt sequence manifest");
    highest = std::max(highest, recorded);
  }
  if (fs::exists(manifest)) {
    std::set<std::uint64_t> live;
    for (const auto& [id, path] : selected) { (void)path; live.insert(id); }
    for (const auto& [id, path] : discovered) if (!live.count(id)) {
      std::error_code ec;
      fs::remove(path, ec);
    }
  }
  tables_ = std::move(loaded);
  next_file_.store(next_id);
  last_sequence_.store(highest);
  // Repair a stale or absent checkpoint from the validated table metadata before
  // the store can accept writes or compaction can discard older versions.
  PersistSequence(highest);
}

AtishaySSTableTableStore::~AtishaySSTableTableStore() = default;

std::shared_ptr<const AtishaySSTableTableStore::TableList> AtishaySSTableTableStore::Current() const {
  std::lock_guard<std::mutex> lock(mu_);
  return tables_;
}

std::shared_ptr<AtishaySSTableTableStore::Table> AtishaySSTableTableStore::WriteTable(
    std::vector<storage::Entry> entries) {
  if (entries.empty()) return {};
  std::sort(entries.begin(), entries.end(), storage::EntryLess);
  const auto id = next_file_.fetch_add(1);
  char filename[32];
  std::snprintf(filename, sizeof(filename), "%06llu.sst", static_cast<unsigned long long>(id));
  const fs::path path = dir_ / filename;
  storage::SSTableWriter writer(path.string());
  for (const auto& entry : entries) writer.Add(entry);
  writer.Finish();
  return std::make_shared<Table>(path);
}

void AtishaySSTableTableStore::PersistManifest(const TableList& tables) const {
  const fs::path temporary = dir_ / "tables.manifest.tmp";
  const fs::path target = dir_ / "tables.manifest";
  std::string contents;
  for (const auto& table : tables) contents += table->path.filename().string() + '\n';
  DurableReplace(temporary, target, contents);
}

void AtishaySSTableTableStore::PersistSequence(SequenceNumber sequence) const {
  const fs::path temporary = dir_ / "sequence.manifest.tmp";
  const fs::path target = dir_ / "sequence.manifest";
  DurableReplace(temporary, target, std::to_string(sequence) + "\n");
}

void AtishaySSTableTableStore::AddTable(const lifecycle::MemTable& memtable) {
  std::vector<storage::Entry> entries;
  SequenceNumber highest = 0;
  memtable.ForEach([&](SequenceNumber seq, ValueType type, const std::string& key, const std::string& value) {
    storage::Entry entry;
    entry.key = key;
    entry.seq = seq;
    entry.type = type == ValueType::kValue ? storage::EntryType::kPut : storage::EntryType::kDelete;
    entry.value = type == ValueType::kValue ? value : std::string();
    entries.push_back(std::move(entry));
    highest = std::max(highest, seq);
  });
  if (entries.empty()) return;
  auto table = WriteTable(std::move(entries));
  std::lock_guard<std::mutex> lock(mu_);
  const auto new_highest = std::max(highest, last_sequence_.load());
  auto next = std::make_shared<TableList>();
  next->push_back(table);
  next->insert(next->end(), tables_->begin(), tables_->end());
  try {
    PersistManifest(*next);
  } catch (...) {
    // The manifest rename may already have happened before a directory-sync
    // error. Keep the immutable file so a possibly-published manifest cannot
    // point at a missing table; startup reconciles unreferenced files safely.
    throw;
  }
  last_sequence_.store(new_highest);
  tables_ = std::move(next);
  // The table footer itself recovers this high-water mark if checkpoint update
  // fails. Never publish a sequence checkpoint before the table manifest: doing
  // so could make startup skip WAL records whose table was not published.
  try { PersistSequence(new_highest); } catch (...) {}
}

LookupResult AtishaySSTableTableStore::Get(const std::string& key, SequenceNumber read_seq) const {
  auto tables = Current();
  std::optional<storage::Entry> newest;
  for (const auto& table : *tables) {
    auto candidate = table->reader->Get(key, read_seq);
    if (candidate && (!newest || candidate->seq > newest->seq)) newest = std::move(candidate);
  }
  if (!newest) return {};
  LookupResult result;
  result.sequence = newest->seq;
  if (newest->type == storage::EntryType::kDelete) result.state = LookupState::kDeleted;
  else {
    result.state = LookupState::kFound;
    result.value = std::move(newest->value);
  }
  return result;
}

bool AtishaySSTableTableStore::NeedsCompaction() const { return Current()->size() >= compaction_trigger_; }

void AtishaySSTableTableStore::Compact(SequenceNumber oldest_snapshot) {
  std::lock_guard<std::mutex> one(compact_mu_);
  auto inputs = Current();
  if (inputs->size() < 2) return;
  std::vector<storage::Entry> all;
  for (const auto& table : *inputs) {
    auto iterator = table->reader->NewIterator();
    for (; iterator->Valid(); iterator->Next()) all.push_back(iterator->entry());
  }
  std::sort(all.begin(), all.end(), storage::EntryLess);
  std::vector<storage::Entry> compacted;
  std::string key;
  bool have_key = false;
  bool floor_kept = false;
  for (auto& entry : all) {
    if (!have_key || key != entry.key) {
      key = entry.key;
      have_key = true;
      floor_kept = false;
    }
    if (entry.seq > oldest_snapshot) {
      compacted.push_back(std::move(entry));
    } else if (!floor_kept) {
      floor_kept = true;
      // All existing tables are inputs. A floor tombstone can therefore be
      // discarded together with the older values it hides.
      if (entry.type == storage::EntryType::kPut) compacted.push_back(std::move(entry));
    }
  }
  auto output = WriteTable(std::move(compacted));
  std::lock_guard<std::mutex> lock(mu_);
  auto next = std::make_shared<TableList>();
  for (const auto& table : *tables_)
    if (std::find(inputs->begin(), inputs->end(), table) == inputs->end()) next->push_back(table);
  if (output) next->push_back(output);
  try {
    // Compaction may discard the table containing the highest sequence (for
    // example, a newest tombstone). Persist that high-water before replacing
    // the old manifest so sequence numbers are never reused after a crash.
    PersistSequence(last_sequence_.load());
    PersistManifest(*next);
  } catch (...) {
    // As above, keep output until startup can decide whether the manifest
    // publication became visible. Old inputs are deliberately not retired.
    throw;
  }
  for (const auto& table : *inputs) table->obsolete.store(true);
  tables_ = std::move(next);
}

std::size_t AtishaySSTableTableStore::TableCount() const { return Current()->size(); }

}  // namespace lsmdb::node

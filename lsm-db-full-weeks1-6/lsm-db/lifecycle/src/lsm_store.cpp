#include "lsmdb/lifecycle/lsm_store.h"

#include <algorithm>
#include <stdexcept>

#include "lsmdb/api/reference_memtable.h"

namespace lsmdb::lifecycle {

using api::LookupState;
using api::ValueType;

LsmStore::LsmStore(std::shared_ptr<TableStore> tables, LsmOptions options)
    : tables_(std::move(tables)), opt_(std::move(options)) {
  if (!tables_) throw std::invalid_argument("LsmStore needs a TableStore");
  if (!opt_.memtable_factory) {
    opt_.memtable_factory = [] { return std::make_shared<api::ReferenceMemTable>(); };
  }
  next_seq_ = opt_.start_sequence + 1;
  last_visible_.store(opt_.start_sequence, std::memory_order_release);
  flushed_up_to_ = opt_.start_sequence;
  active_ = opt_.memtable_factory();
  flush_thread_ = std::thread([this] { FlushLoop(); });
  compaction_thread_ = std::thread([this] { CompactionLoop(); });
}

LsmStore::~LsmStore() {
  {
    std::lock_guard<std::mutex> l(bg_mu_);
    stopping_ = true;
  }
  bg_cv_.notify_all();
  flush_thread_.join();
  compaction_thread_.join();
}

// ---------------------------------------------------------------- writes

void LsmStore::RotateLocked(std::unique_lock<std::mutex>& bg) {
  bool counted = false;
  while (imm_ && bg_error_.empty() && !stopping_) {
    if (!counted) { ++stats_.write_stalls; counted = true; }
    bg_cv_.wait(bg);                       // backpressure: previous flush still running
  }
  if (!bg_error_.empty()) throw std::runtime_error("flush failed: " + bg_error_);
  if (stopping_) throw std::runtime_error("store is shutting down");
  imm_ = std::move(active_);
  imm_last_seq_ = last_visible_.load(std::memory_order_relaxed);
  active_ = opt_.memtable_factory();
  ++stats_.rotations;
  bg_cv_.notify_all();                     // wake the flush worker
}

void LsmStore::Write(ValueType type, const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> w(write_mu_);
  if (active_->ApproximateMemoryUsage() >= opt_.memtable_bytes && active_->EntryCount() > 0) {
    std::unique_lock<std::mutex> bg(bg_mu_);
    RotateLocked(bg);
  }
  const SequenceNumber seq = next_seq_;
  active_->Add(seq, type, key, value);     // throws -> sequence not consumed
  ++next_seq_;
  last_visible_.store(seq, std::memory_order_release);
}

void LsmStore::Put(const std::string& key, const std::string& value) {
  Write(ValueType::kValue, key, value);
}
void LsmStore::Delete(const std::string& key) { Write(ValueType::kTombstone, key, std::string()); }

void LsmStore::Restore(SequenceNumber seq, ValueType type, const std::string& key,
                       const std::string& value) {
  std::lock_guard<std::mutex> w(write_mu_);
  if (seq < next_seq_) throw std::logic_error("Restore: sequence numbers must increase");
  active_->Add(seq, type, key, value);
  next_seq_ = seq + 1;
  last_visible_.store(seq, std::memory_order_release);
}

// ---------------------------------------------------------------- reads

LsmStore::Pinned LsmStore::PinState(bool register_snapshot) {
  std::lock_guard<std::mutex> l(bg_mu_);
  Pinned p;
  p.active = active_;
  p.imm = imm_;
  // Pointers and sequence are captured together: every entry <= seq is in active, imm or tables.
  p.seq = last_visible_.load(std::memory_order_acquire);
  if (register_snapshot) snapshots_.insert(p.seq);
  return p;
}

api::LookupResult LsmStore::Lookup(const std::string& key, const Pinned& p) const {
  api::LookupResult r = p.active->Get(key, p.seq);
  if (r.state != LookupState::kNotFound) return r;
  if (p.imm) {
    r = p.imm->Get(key, p.seq);
    if (r.state != LookupState::kNotFound) return r;
  }
  return tables_->Get(key, p.seq);
}

std::optional<std::string> LsmStore::Get(const std::string& key) {
  Pinned p = PinState(true);
  struct Unpin {
    LsmStore* s; SequenceNumber q;
    ~Unpin() { s->ReleaseSnapshot(q); }
  } unpin{this, p.seq};
  api::LookupResult r = Lookup(key, p);
  if (r.state == LookupState::kFound) return std::move(r.value);
  return std::nullopt;
}

api::LookupResult LsmStore::GetAt(const std::string& key, SequenceNumber read_seq) {
  Pinned p = PinState(false);
  p.seq = std::min(read_seq, p.seq);   // cannot read the future
  return Lookup(key, p);
}

LsmStore::Snapshot LsmStore::AcquireSnapshot() {
  Pinned p = PinState(true);
  return Snapshot(this, p.seq);
}

void LsmStore::ReleaseSnapshot(SequenceNumber seq) {
  std::lock_guard<std::mutex> l(bg_mu_);
  auto it = snapshots_.find(seq);
  if (it != snapshots_.end()) snapshots_.erase(it);
}

LsmStore::Snapshot& LsmStore::Snapshot::operator=(Snapshot&& o) noexcept {
  if (this != &o) {
    Release();
    store_ = o.store_;
    seq_ = o.seq_;
    o.store_ = nullptr;
  }
  return *this;
}

void LsmStore::Snapshot::Release() {
  if (store_) store_->ReleaseSnapshot(seq_);
  store_ = nullptr;
}

// ---------------------------------------------------------------- control

SequenceNumber LsmStore::FlushedUpTo() const {
  std::lock_guard<std::mutex> l(bg_mu_);
  return flushed_up_to_;
}

LsmStats LsmStore::stats() const {
  std::lock_guard<std::mutex> l(bg_mu_);
  return stats_;
}

std::string LsmStore::background_error() const {
  std::lock_guard<std::mutex> l(bg_mu_);
  return bg_error_;
}

void LsmStore::Flush() {
  SequenceNumber target;
  {
    std::lock_guard<std::mutex> w(write_mu_);
    target = last_visible_.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> bg(bg_mu_);
    if (active_->EntryCount() > 0) RotateLocked(bg);
  }
  std::unique_lock<std::mutex> bg(bg_mu_);
  bg_cv_.wait(bg, [&] { return flushed_up_to_ >= target || !bg_error_.empty() || stopping_; });
  if (!bg_error_.empty()) throw std::runtime_error("flush failed: " + bg_error_);
  if (flushed_up_to_ < target) throw std::runtime_error("store is shutting down");
}

void LsmStore::WaitForIdle() {
  std::unique_lock<std::mutex> bg(bg_mu_);
  bg_cv_.wait(bg, [&] {
    if (stopping_ || !bg_error_.empty()) return true;
    const bool compaction_idle =
        !compaction_error_.empty() || (!compaction_requested_ && !compacting_);
    return !imm_ && compaction_idle;
  });
}

// ---------------------------------------------------------------- workers

void LsmStore::FlushLoop() {
  std::unique_lock<std::mutex> l(bg_mu_);
  for (;;) {
    bg_cv_.wait(l, [&] { return stopping_ || (imm_ && bg_error_.empty()); });
    if (stopping_) return;
    std::shared_ptr<MemTable> imm = imm_;
    const SequenceNumber upto = imm_last_seq_;
    l.unlock();

    std::string err;
    bool more_compaction = false;
    try {
      tables_->AddTable(*imm);                 // publish the table BEFORE dropping imm_
      more_compaction = tables_->NeedsCompaction();
    } catch (const std::exception& e) {
      err = e.what();
      if (err.empty()) err = "unknown flush error";
    }

    // The table is durable now. Tell the WAL owner BEFORE publishing the new flushed_up_to_, so
    // Flush() returning means the callback has already run.
    if (err.empty() && opt_.on_flushed) {
      try { opt_.on_flushed(upto); } catch (...) {}   // callback must not break the worker
    }

    l.lock();
    if (!err.empty()) {
      bg_error_ = err;                          // imm_ is kept, so its data stays readable
      bg_cv_.notify_all();
      continue;
    }
    imm_.reset();
    flushed_up_to_ = upto;
    ++stats_.flushes;
    if (more_compaction) compaction_requested_ = true;
    bg_cv_.notify_all();
  }
}

void LsmStore::CompactionLoop() {
  std::unique_lock<std::mutex> l(bg_mu_);
  for (;;) {
    bg_cv_.wait(l, [&] {
      return stopping_ || (compaction_requested_ && compaction_error_.empty());
    });
    if (stopping_) return;
    compaction_requested_ = false;
    compacting_ = true;
    // Oldest sequence any running or future read can use.
    SequenceNumber oldest = last_visible_.load(std::memory_order_acquire);
    if (!snapshots_.empty()) oldest = std::min(oldest, *snapshots_.begin());
    l.unlock();

    std::string err;
    bool more = false;
    try {
      tables_->Compact(oldest);
      more = tables_->NeedsCompaction();
    } catch (const std::exception& e) {
      err = e.what();
      if (err.empty()) err = "unknown compaction error";
    }

    l.lock();
    compacting_ = false;
    if (err.empty()) {
      ++stats_.compactions;
      compaction_requested_ = more;
    } else {
      compaction_error_ = err;                  // stop scheduling; writes keep working
    }
    bg_cv_.notify_all();
  }
}

}  // namespace lsmdb::lifecycle

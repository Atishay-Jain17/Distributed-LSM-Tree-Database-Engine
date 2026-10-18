#include "lsmdb/storage/sstable_reader.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>

#include "lsmdb/storage/coding.h"
#include "lsmdb/storage/crc32.h"

namespace lsmdb::storage {
namespace {
constexpr std::uint64_t kReadChunk = 64 * 1024;
}

std::unique_ptr<SSTableReader> SSTableReader::Open(const std::string& path) {
  std::unique_ptr<SSTableReader> r(new SSTableReader());
  r->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (r->fd_ < 0) throw std::runtime_error("open " + path + ": " + std::strerror(errno));

  struct stat st;
  if (::fstat(r->fd_, &st) != 0) throw std::runtime_error("fstat " + path);
  const std::uint64_t size = static_cast<std::uint64_t>(st.st_size);
  if (size < kFooterSize || size % kBlockAlignment != 0) {
    throw CorruptionError(path + ": file size is not a valid SSTable size");
  }
  std::string page;
  r->ReadAt(size - kFooterSize, kFooterSize, &page);
  r->footer_ = DecodeFooter(page.data());

  const Footer& f = r->footer_;
  if (f.entry_count == 0 || AlignUp(f.data_size) > size - kFooterSize) {
    throw CorruptionError(path + ": footer geometry inconsistent with file size");
  }
  std::uint64_t index_end = AlignUp(f.data_size);
  if (f.index_size == 0) {
    if (f.index_offset != 0) throw CorruptionError(path + ": invalid empty index geometry");
  } else {
    if (f.index_offset != AlignUp(f.data_size) || f.index_size % kBlockAlignment != 0 ||
        f.index_size > 64 * 1024 * 1024 || f.index_offset > size - kFooterSize ||
        f.index_size > size - kFooterSize - f.index_offset) {
      throw CorruptionError(path + ": invalid sparse index section geometry");
    }
    std::string index_data;
    r->ReadAt(f.index_offset, f.index_size, &index_data);
    std::istringstream index_stream(index_data, std::ios::binary | std::ios::in);
    const lsm::Status index_status = r->sparse_index_.Deserialize(index_stream);
    if (!index_status.ok() || r->sparse_index_.Empty())
      throw CorruptionError(path + ": invalid sparse index: " + index_status.ToString());
    const auto& entries = r->sparse_index_.Entries();
    if (r->sparse_index_.MinKey() != f.min_key || r->sparse_index_.MaxKey() != f.max_key)
      throw CorruptionError(path + ": sparse index/table key bounds disagree");
    if (entries.front().offset != 0) throw CorruptionError(path + ": sparse index does not start at data offset zero");
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const std::uint64_t expected_end = i + 1 < entries.size() ? entries[i + 1].offset : f.data_size;
      if (entries[i].size == 0 || entries[i].offset >= f.data_size ||
          entries[i].offset + entries[i].size != expected_end) {
        throw CorruptionError(path + ": sparse index block geometry is inconsistent");
      }
    }
    if (!r->sparse_index_.MayContainRange(f.min_key) || !r->sparse_index_.MayContainRange(f.max_key))
      throw CorruptionError(path + ": sparse index key range does not cover table range");
    index_end = f.index_offset + f.index_size;
  }
  if (f.filter_size == 0) {
    if (f.filter_offset != 0 || index_end + kFooterSize != size)
      throw CorruptionError(path + ": invalid empty Bloom filter geometry");
  } else {
    if (f.filter_offset != index_end || f.filter_size % kBlockAlignment != 0 ||
        f.filter_size > 64 * 1024 * 1024 || f.filter_offset > size - kFooterSize ||
        f.filter_size != size - kFooterSize - f.filter_offset)
      throw CorruptionError(path + ": invalid Bloom filter section geometry");
    std::string filter_data;
    r->ReadAt(f.filter_offset, f.filter_size, &filter_data);
    try {
      r->bloom_filter_ = BloomFilter::Deserialize(filter_data);
    } catch (const std::exception& e) {
      throw CorruptionError(path + ": invalid Bloom filter: " + e.what());
    }
  }
  r->meta_.path = path;
  r->meta_.file_size = size;
  r->meta_.entry_count = f.entry_count;
  r->meta_.min_key = f.min_key;
  r->meta_.max_key = f.max_key;
  r->meta_.min_seq = f.min_seq;
  r->meta_.max_seq = f.max_seq;
  return r;
}

SSTableReader::~SSTableReader() {
  if (fd_ >= 0) ::close(fd_);
}

void SSTableReader::ReadAt(std::uint64_t offset, std::uint64_t n, std::string* out) const {
  out->resize(n);
  std::uint64_t done = 0;
  while (done < n) {
    ssize_t r = ::pread(fd_, out->data() + done, n - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("pread: ") + std::strerror(errno));
    }
    if (r == 0) throw CorruptionError("unexpected end of SSTable file");
    done += static_cast<std::uint64_t>(r);
  }
}

void SSTableReader::VerifyChecksum() const {
  std::uint32_t crc = 0;
  std::string chunk;
  for (std::uint64_t off = 0; off < footer_.data_size; off += kReadChunk) {
    std::uint64_t n = std::min<std::uint64_t>(kReadChunk, footer_.data_size - off);
    ReadAt(off, n, &chunk);
    crc = Crc32Update(crc, chunk.data(), chunk.size());
  }
  if (crc != footer_.data_crc) throw CorruptionError(meta_.path + ": data checksum mismatch");
}

std::optional<Entry> SSTableReader::Get(const std::string& key, std::uint64_t snapshot_seq) const {
  if (key < footer_.min_key || key > footer_.max_key) return std::nullopt;
  if (bloom_filter_ && !bloom_filter_->MayContain(key)) return std::nullopt;
  if (!sparse_index_.Empty()) {
    lsm::IndexEntry block;
    if (!sparse_index_.FindCandidateBlock(key, &block)) return std::nullopt;
    const std::uint64_t end = block.offset + block.size;
    std::uint64_t offset = block.offset;
    while (offset < end) {
      if (end - offset < kRecordHeaderSize) throw CorruptionError("truncated indexed SSTable record header");
      std::string header;
      ReadAt(offset, kRecordHeaderSize, &header);
      const std::uint32_t klen = DecodeFixed32(header.data());
      const std::uint32_t vlen = DecodeFixed32(header.data() + 4);
      const std::uint64_t seq = DecodeFixed64(header.data() + 8);
      const std::uint8_t type = static_cast<std::uint8_t>(header[16]);
      const std::uint64_t record_size = kRecordHeaderSize + static_cast<std::uint64_t>(klen) + vlen;
      if (klen == 0 || klen > kMaxSSTableKeyBytes || vlen > kMaxSSTableValueBytes || type > 1 || record_size > end - offset)
        throw CorruptionError("malformed record in indexed SSTable block");
      std::string record;
      ReadAt(offset + kRecordHeaderSize, static_cast<std::uint64_t>(klen) + vlen, &record);
      const std::string found_key(record.data(), klen);
      if (found_key > key) return std::nullopt;
      if (found_key == key && seq <= snapshot_seq) {
        Entry entry;
        entry.key = found_key;
        entry.value.assign(record.data() + klen, vlen);
        entry.seq = seq;
        entry.type = static_cast<EntryType>(type);
        return entry;
      }
      offset += record_size;
    }
    return std::nullopt;
  }
  auto it = NewIterator();
  for (; it->Valid(); it->Next()) {
    const Entry& e = it->entry();
    int c = e.key.compare(key);
    if (c > 0) break;  // passed the key: not present
    if (c == 0 && e.seq <= snapshot_seq) return e;
  }
  return std::nullopt;
}

std::unique_ptr<SSTableReader::Iterator> SSTableReader::NewIterator() const {
  return std::unique_ptr<Iterator>(new Iterator(this));
}

SSTableReader::Iterator::Iterator(const SSTableReader* reader) : reader_(reader) { Parse(); }

void SSTableReader::Iterator::Next() { Parse(); }

void SSTableReader::Iterator::Refill(std::uint64_t need) {
  const std::uint64_t limit = reader_->footer_.data_size;
  if (next_offset_ + need > limit) throw CorruptionError("record extends past data section");
  const std::uint64_t buf_end = buf_offset_ + buf_.size();
  if (next_offset_ >= buf_offset_ && next_offset_ + need <= buf_end) return;
  std::uint64_t n = std::min<std::uint64_t>(std::max(need, kReadChunk), limit - next_offset_);
  reader_->ReadAt(next_offset_, n, &buf_);
  buf_offset_ = next_offset_;
}

void SSTableReader::Iterator::Parse() {
  if (next_offset_ >= reader_->footer_.data_size) {
    valid_ = false;
    return;
  }
  Refill(kRecordHeaderSize);
  const char* h = buf_.data() + (next_offset_ - buf_offset_);
  const std::uint32_t klen = DecodeFixed32(h);
  const std::uint32_t vlen = DecodeFixed32(h + 4);
  const std::uint64_t seq = DecodeFixed64(h + 8);
  const std::uint8_t type = static_cast<std::uint8_t>(h[16]);
  if (klen == 0 || klen > kMaxSSTableKeyBytes || vlen > kMaxSSTableValueBytes || type > 1) {
    throw CorruptionError("malformed SSTable record header");
  }
  const std::uint64_t total = kRecordHeaderSize + klen + vlen;
  Refill(total);
  h = buf_.data() + (next_offset_ - buf_offset_);
  entry_.key.assign(h + kRecordHeaderSize, klen);
  entry_.value.assign(h + kRecordHeaderSize + klen, vlen);
  entry_.seq = seq;
  entry_.type = static_cast<EntryType>(type);
  next_offset_ += total;
  valid_ = true;
}

}  // namespace lsmdb::storage

#include "lsmdb/storage/sstable_reader.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

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
  if (AlignUp(f.data_size) + kFooterSize > size || f.entry_count == 0) {
    throw CorruptionError(path + ": footer geometry inconsistent with file size");
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

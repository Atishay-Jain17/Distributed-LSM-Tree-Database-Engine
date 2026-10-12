#include "lsmdb/storage/sstable_writer.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "lsmdb/storage/coding.h"
#include "lsmdb/storage/crc32.h"
#include "lsmdb/storage/sstable_format.h"

namespace lsmdb::storage {
namespace {
constexpr std::size_t kFlushThreshold = 64 * 1024;

[[noreturn]] void ThrowErrno(const std::string& what) {
  throw std::runtime_error(what + ": " + std::strerror(errno));
}
}  // namespace

SSTableWriter::SSTableWriter(std::string path)
    : path_(std::move(path)), tmp_path_(path_ + ".tmp") {
  fd_ = ::open(tmp_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd_ < 0) ThrowErrno("open " + tmp_path_);
}

SSTableWriter::~SSTableWriter() {
  if (fd_ >= 0) ::close(fd_);
  if (!finished_) {
    std::error_code ec;
    std::filesystem::remove(tmp_path_, ec);
  }
}

void SSTableWriter::WriteAll(const char* data, std::size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd_, data, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      ThrowErrno("write " + tmp_path_);
    }
    data += w;
    n -= static_cast<std::size_t>(w);
  }
}

void SSTableWriter::FlushBuffer() {
  if (buffer_.empty()) return;
  WriteAll(buffer_.data(), buffer_.size());
  crc_ = Crc32Update(crc_, buffer_.data(), buffer_.size());
  buffer_.clear();
}

void SSTableWriter::Add(const Entry& e) {
  if (finished_) throw std::logic_error("SSTableWriter already finished");
  if (e.key.empty() || e.key.size() > kMaxSSTableKeyBytes) {
    throw std::invalid_argument("SSTable key must be 1.." + std::to_string(kMaxSSTableKeyBytes) + " bytes");
  }
  if (e.value.size() > kMaxSSTableValueBytes) throw std::invalid_argument("SSTable value too large");
  if (e.type == EntryType::kDelete && !e.value.empty()) {
    throw std::invalid_argument("tombstone must have an empty value");
  }
  if (entry_count_ > 0) {
    int c = e.key.compare(last_key_);
    if (c < 0 || (c == 0 && e.seq >= last_seq_)) {
      throw std::invalid_argument("entries must be added in strictly increasing (key asc, seq desc) order");
    }
  }

  PutFixed32(&buffer_, static_cast<std::uint32_t>(e.key.size()));
  PutFixed32(&buffer_, static_cast<std::uint32_t>(e.value.size()));
  PutFixed64(&buffer_, e.seq);
  buffer_.push_back(static_cast<char>(e.type));
  buffer_.append(e.key);
  buffer_.append(e.value);
  data_bytes_ += kRecordHeaderSize + e.key.size() + e.value.size();

  if (entry_count_ == 0) {
    min_key_ = e.key;
    min_seq_ = max_seq_ = e.seq;
  } else {
    min_seq_ = std::min(min_seq_, e.seq);
    max_seq_ = std::max(max_seq_, e.seq);
  }
  last_key_ = e.key;
  last_seq_ = e.seq;
  ++entry_count_;
  if (buffer_.size() >= kFlushThreshold) FlushBuffer();
}

SSTableMetadata SSTableWriter::Finish() {
  if (finished_) throw std::logic_error("SSTableWriter already finished");
  if (entry_count_ == 0) throw std::logic_error("cannot finish an empty SSTable");
  FlushBuffer();

  const std::size_t padded = AlignUp(data_bytes_);
  std::string pad(padded - data_bytes_, '\0');
  WriteAll(pad.data(), pad.size());

  Footer f;
  f.data_size = data_bytes_;
  f.entry_count = entry_count_;
  f.min_seq = min_seq_;
  f.max_seq = max_seq_;
  f.data_crc = crc_;
  f.min_key = min_key_;
  f.max_key = last_key_;  // keys ascend, so the last key added is the maximum
  std::string footer = EncodeFooter(f);
  WriteAll(footer.data(), footer.size());

  if (::fsync(fd_) != 0) ThrowErrno("fsync " + tmp_path_);
  if (::close(fd_) != 0) {
    fd_ = -1;
    ThrowErrno("close " + tmp_path_);
  }
  fd_ = -1;
  if (::rename(tmp_path_.c_str(), path_.c_str()) != 0) ThrowErrno("rename " + tmp_path_);
  finished_ = true;

  // Make the rename itself durable.
  std::string dir = std::filesystem::path(path_).parent_path().string();
  if (dir.empty()) dir = ".";
  int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }

  SSTableMetadata meta;
  meta.path = path_;
  meta.file_size = padded + kFooterSize;
  meta.entry_count = entry_count_;
  meta.min_key = f.min_key;
  meta.max_key = f.max_key;
  meta.min_seq = min_seq_;
  meta.max_seq = max_seq_;
  return meta;
}

}  // namespace lsmdb::storage

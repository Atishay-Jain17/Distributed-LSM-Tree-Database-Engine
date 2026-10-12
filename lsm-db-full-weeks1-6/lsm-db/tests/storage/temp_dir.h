#pragma once
#include <stdlib.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace lsmdb::testing_support {

// Creates a unique directory under /tmp and removes it (recursively) on destruction.
class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/lsmdb_test_XXXXXX";
    if (!mkdtemp(tmpl)) throw std::runtime_error("mkdtemp failed");
    path_ = tmpl;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::string& path() const { return path_; }
  std::string File(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

}  // namespace lsmdb::testing_support

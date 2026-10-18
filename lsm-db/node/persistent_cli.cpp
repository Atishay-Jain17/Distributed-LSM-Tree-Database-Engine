#include "lsmdb/node/persistent_backend.h"

#include <filesystem>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void PrintHelp() {
  std::cout << "Commands:\n"
            << "  PUT <key> <value>   Store a value (quote fields containing spaces)\n"
            << "  GET <key>           Read a value\n"
            << "  DELETE <key>        Delete a key (DEL is an alias)\n"
            << "  INFO                Show sequence, flush, and compaction counters\n"
            << "  FLUSH               Flush data to tables and checkpoint the WAL\n"
            << "  HELP                Show this help\n"
            << "  EXIT                Flush and close the database\n";
}

bool ParseSize(const std::string& text, std::size_t& value) {
  try {
    std::size_t used = 0;
    const auto parsed = std::stoull(text, &used);
    if (used != text.size() || parsed == 0 || parsed > std::numeric_limits<std::size_t>::max())
      return false;
    value = static_cast<std::size_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path data_dir = "data/local-cli";
  lsmdb::lifecycle::LsmOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "--data-dir" || arg == "--data_dir") && i + 1 < argc) {
      data_dir = argv[++i];
    } else if (arg == "--memtable-bytes" && i + 1 < argc) {
      if (!ParseSize(argv[++i], options.memtable_bytes)) {
        std::cerr << "Invalid --memtable-bytes; expected a positive integer.\n";
        return 2;
      }
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: lsmdb_local_cli [--data-dir PATH] [--memtable-bytes N]\n";
      return 0;
    } else {
      std::cerr << "Unknown or incomplete option: " << arg << "\n";
      return 2;
    }
  }

  try {
    auto db = lsmdb::node::PersistentBackend::Open(data_dir, options);
    const auto& recovered = db->recovery_stats();
    std::cout << "Persistent LSM CLI ready\n"
              << "Data directory: " << std::filesystem::absolute(data_dir).string() << "\n"
              << "Recovered WAL records: " << recovered.records_applied
              << ", table records skipped: " << recovered.records_skipped
              << ", last sequence: " << recovered.last_sequence
              << ", torn tail dropped: " << (recovered.dropped_torn_tail ? "yes" : "no") << "\n";
    PrintHelp();

    std::string line;
    while (std::cout << "lsm> " && std::getline(std::cin, line)) {
      std::istringstream input(line);
      std::string command;
      if (!(input >> command)) continue;
      for (char& ch : command) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

      try {
        std::string key, value, extra;
        if (command == "PUT") {
          if (!(input >> std::quoted(key) >> std::quoted(value)) || (input >> extra) || key.empty())
            throw std::invalid_argument("usage: PUT <key> <value> (quote fields containing spaces)");
          db->Put(key, value);
          std::cout << "OK sequence=" << db->store().LastSequence() << "\n";
        } else if (command == "GET") {
          if (!(input >> std::quoted(key)) || (input >> extra) || key.empty())
            throw std::invalid_argument("usage: GET <key>");
          const auto result = db->Get(key);
          if (result) std::cout << "VALUE " << std::quoted(*result) << "\n";
          else std::cout << "NOT_FOUND\n";
        } else if (command == "DELETE" || command == "DEL") {
          if (!(input >> std::quoted(key)) || (input >> extra) || key.empty())
            throw std::invalid_argument("usage: DELETE <key>");
          db->Delete(key);
          std::cout << "OK sequence=" << db->store().LastSequence() << "\n";
        } else if (command == "INFO") {
          if (input >> extra) throw std::invalid_argument("usage: INFO");
          const auto stats = db->store().stats();
          std::cout << "INFO sequence=" << db->store().LastSequence()
                    << " flushed_up_to=" << db->store().FlushedUpTo()
                    << " rotations=" << stats.rotations << " flushes=" << stats.flushes
                    << " compactions=" << stats.compactions << " write_stalls=" << stats.write_stalls
                    << " wal_bytes=";
          const auto wal = data_dir / "wal.log";
          std::cout << (std::filesystem::exists(wal) ? std::filesystem::file_size(wal) : 0) << "\n";
        } else if (command == "FLUSH") {
          if (input >> extra) throw std::invalid_argument("usage: FLUSH");
          db->Flush();
          db->store().WaitForIdle();
          std::cout << "OK flushed_up_to=" << db->store().FlushedUpTo() << "\n";
        } else if (command == "HELP") {
          PrintHelp();
        } else if (command == "EXIT" || command == "QUIT") {
          if (input >> extra) throw std::invalid_argument("usage: EXIT");
          db->Flush();
          db->store().WaitForIdle();
          std::cout << "Database closed cleanly.\n";
          return 0;
        } else {
          std::cout << "ERROR unknown command; type HELP\n";
        }
      } catch (const std::exception& error) {
        std::cout << "ERROR " << error.what() << "\n";
      }
    }
    db->Flush();
    db->store().WaitForIdle();
    std::cout << "Database closed cleanly.\n";
  } catch (const std::exception& error) {
    std::cerr << "Startup failed: " << error.what() << "\n";
    return 1;
  }
  return 0;
}

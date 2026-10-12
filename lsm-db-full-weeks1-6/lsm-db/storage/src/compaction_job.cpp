#include "lsmdb/storage/compaction_job.h"

#include <filesystem>
#include <memory>
#include <queue>
#include <vector>

#include "lsmdb/storage/sstable_writer.h"

namespace lsmdb::storage {
namespace {

struct Source {
  std::shared_ptr<SSTableReader> reader;  // keeps the reader alive for the iterator
  std::unique_ptr<SSTableReader::Iterator> it;
};

}  // namespace

CompactionStats RunCompaction(const CompactionTask& task, VersionSet& versions,
                              const CompactionOptions& options, std::uint64_t oldest_snapshot_seq) {
  CompactionStats stats;
  std::vector<std::shared_ptr<FileMeta>> all = task.inputs;
  all.insert(all.end(), task.next_inputs.begin(), task.next_inputs.end());
  stats.input_files = all.size();

  std::vector<std::shared_ptr<FileMeta>> outputs;
  std::unique_ptr<SSTableWriter> writer;
  std::uint64_t writer_number = 0;

  try {
    std::vector<Source> sources;
    for (const auto& f : all) {
      Source s;
      s.reader = f->Reader();
      s.reader->VerifyChecksum();  // never merge damaged data into new files
      s.it = s.reader->NewIterator();
      sources.push_back(std::move(s));
    }

    // Min-heap on EntryLess: smallest key first, newest version first within a key.
    auto after = [&](std::size_t a, std::size_t b) {
      return EntryLess(sources[b].it->entry(), sources[a].it->entry());
    };
    std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(after)> heap(after);
    for (std::size_t i = 0; i < sources.size(); ++i) {
      if (sources[i].it->Valid()) heap.push(i);
    }

    auto finish_writer = [&] {
      if (!writer) return;
      SSTableMetadata meta = writer->Finish();
      outputs.push_back(std::make_shared<FileMeta>(writer_number, task.output_level, std::move(meta)));
      writer.reset();
    };

    bool have_last = false, kept_visible = false;
    std::string last_key, current_key, last_written_key;
    std::uint64_t last_seq = 0;

    while (!heap.empty()) {
      std::size_t idx = heap.top();
      heap.pop();
      Entry e = sources[idx].it->entry();
      sources[idx].it->Next();
      if (sources[idx].it->Valid()) heap.push(idx);
      ++stats.entries_in;

      if (have_last && e.key == last_key && e.seq == last_seq) {  // exact duplicate
        ++stats.entries_dropped;
        continue;
      }
      have_last = true;
      last_key = e.key;
      last_seq = e.seq;

      if (e.key != current_key || stats.entries_in == 1) {
        current_key = e.key;
        kept_visible = false;
      }

      bool drop = false;
      if (e.seq > oldest_snapshot_seq) {
        // visible only to newer snapshots: must keep
      } else if (!kept_visible) {
        kept_visible = true;  // newest version the oldest snapshot can see
        if (e.type == EntryType::kDelete && !KeyMayExistBelow(*task.base, e.key, task.output_level)) {
          drop = true;  // nothing deeper for this tombstone to hide
        }
      } else {
        drop = true;  // shadowed by a newer visible version
      }
      if (drop) {
        ++stats.entries_dropped;
        continue;
      }

      // Roll to a new file only on a key boundary, so one key never straddles two files.
      if (writer && writer->data_bytes() >= options.target_file_bytes && e.key != last_written_key) {
        finish_writer();
      }
      if (!writer) {
        writer_number = versions.NewFileNumber();
        writer = std::make_unique<SSTableWriter>(versions.FilePath(task.output_level, writer_number));
      }
      writer->Add(e);
      last_written_key = e.key;
      ++stats.entries_out;
    }
    finish_writer();

    VersionEdit edit;
    edit.added = outputs;
    edit.removed = all;
    versions.Apply(edit);
  } catch (...) {
    writer.reset();                              // removes the unfinished temp file
    for (auto& o : outputs) o->MarkObsolete();   // finished-but-unpublished outputs are deleted
    outputs.clear();
    throw;
  }
  stats.output_files = outputs.size();
  return stats;
}

}  // namespace lsmdb::storage

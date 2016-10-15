// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "re2/re2.h"
#include <assert.h>

#include "bloaty.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define CHECK_SYSCALL(call) \
  if (call < 0) { \
    perror(#call " " __FILE__ ":" TOSTRING(__LINE__)); \
    exit(1); \
  }

template<typename To, typename From>     // use like this: down_cast<T*>(foo);
inline To down_cast(From* f) {           // so we only accept pointers
  static_assert(
      (std::is_base_of<From, typename std::remove_pointer<To>::type>::value),
      "target type not derived from source type");

#if !defined(__GNUC__) || defined(__GXX_RTTI)
  assert(f == nullptr || dynamic_cast<To>(f) != nullptr);
#endif  // !defined(__GNUC__) || defined(__GXX_RTTI)

  return static_cast<To>(f);
}

namespace bloaty {

std::string* name_path;
const size_t kMaxLabelLen = 80;
bool warnings = false;

void PrintSpaces(size_t n) {
  for (size_t i = 0; i < n; i++) {
    printf(" ");
  }
}

double Percent(ssize_t part, size_t whole) {
  return static_cast<double>(part) / static_cast<double>(whole) * 100;
}

std::string FixedWidthString(const std::string& input, size_t size) {
  if (input.size() < size) {
    std::string ret = input;
    while (ret.size() < size) {
      ret += " ";
    }
    return ret;
  } else {
    return input.substr(0, size);
  }
}

std::string LeftPad(const std::string& input, size_t size) {
  std::string ret = input;
  while (ret.size() < size) {
    ret = " " + ret;
  }

  return ret;
}

std::string DoubleStringPrintf(const char *fmt, double d) {
  char buf[1024];
  snprintf(buf, sizeof(buf), fmt, d);
  return std::string(buf);
}

std::string SiPrint(ssize_t size, bool force_sign) {
  const char *prefixes[] = {"", "Ki", "Mi", "Gi", "Ti"};
  int n = 0;
  double size_d = size;
  while (fabs(size_d) > 1024) {
    size_d /= 1024;
    n++;
  }

  std::string ret;

  if (fabs(size_d) > 100 || n == 0) {
    ret = std::to_string(static_cast<ssize_t>(size_d)) + prefixes[n];
    if (force_sign && size > 0) {
      ret = "+" + ret;
    }
  } else if (fabs(size_d) > 10) {
    if (force_sign) {
      ret = DoubleStringPrintf("%+0.1f", size_d) + prefixes[n];
    } else {
      ret = DoubleStringPrintf("%0.1f", size_d) + prefixes[n];
    }
  } else {
    if (force_sign) {
      ret = DoubleStringPrintf("%+0.2f", size_d) + prefixes[n];
    } else {
      ret = DoubleStringPrintf("%0.2f", size_d) + prefixes[n];
    }
  }

  return LeftPad(ret, 7);
}

std::string PercentString(double percent, bool diff_mode) {
  if (diff_mode) {
    if (percent == 0 || isnan(percent)) {
      return " [ = ]";
    } else if (percent == -100) {
      return " [DEL]";
    } else if (isinf(percent)) {
      return " [NEW]";
    } else {
      // We want to keep this fixed-width even if the percent is very large.
      std::string str;
      if (percent > 1000) {
        int digits = log10(percent) - 1;
        str = DoubleStringPrintf("%+2.0f", percent / pow(10, digits)) + "e" +
              std::to_string(digits) + "%";
      } else if (percent > 10) {
        str = DoubleStringPrintf("%+4.0f%%", percent);
      } else {
        str = DoubleStringPrintf("%+5.1F%%", percent);
      }

      return LeftPad(str, 6);
    }
  } else {
    return DoubleStringPrintf("%5.1F%%", percent);
  }
}

void Split(const std::string& str, char delim, std::vector<std::string>* out) {
  std::stringstream stream(str);
  std::string item;
  while (std::getline(stream, item, delim)) {
    out->push_back(item);
  }
}

uint64_t GetFileSize(const std::string& filename) {
  FILE* file = fopen(filename.c_str(), "rb");
  if (!file) {
    std::cerr << "Couldn't get file size for: " << filename << "\n";
    exit(1);
  }
  fseek(file, 0L, SEEK_END);
  long ret = ftell(file);
  fclose(file);
  return ret;
}


// LineReader / LineIterator ///////////////////////////////////////////////////

// Convenience code for iterating over lines of a pipe.

LineReader::LineReader(LineReader&& other) {
  Close();

  file_ = other.file_;
  pclose_ = other.pclose_;

  other.file_ = nullptr;
}

void LineReader::Close() {
  if (!file_) return;

  if (pclose_) {
    pclose(file_);
  } else {
    fclose(file_);
  }
}

void LineReader::Next() {
  char buf[256];
  line_.clear();
  do {
    if (!fgets(buf, sizeof(buf), file_)) {
      if (feof(file_)) {
        eof_ = true;
        break;
      } else {
        std::cerr << "Error reading from file.\n";
        exit(1);
      }
    }
    line_.append(buf);
  } while(!eof_ && line_[line_.size() - 1] != '\n');

  if (!eof_) {
    line_.resize(line_.size() - 1);
  }
}

LineIterator LineReader::begin() { return LineIterator(this); }
LineIterator LineReader::end() { return LineIterator(nullptr); }

LineReader ReadLinesFromPipe(const std::string& cmd) {
  FILE* pipe = popen(cmd.c_str(), "r");

  if (!pipe) {
    std::cerr << "Failed to run command: " << cmd << "\n";
    exit(1);
  }

  return LineReader(pipe, true);
}


// NameStripper ////////////////////////////////////////////////////////////////

// C++ Symbol names can get really long because they include all the parameter
// types.  For example:
//
// bloaty::RangeMap::ComputeRollup(std::vector<bloaty::RangeMap const*, std::allocator<bloaty::RangeMap const*> > const&, bloaty::Rollup*)
//
// In most cases, we can strip all of the parameter info.  We only need to keep
// it in the case of overloaded functions.  This class can do the stripping, but
// the caller needs to verify that the stripped name is still unique within the
// binary, otherwise the full name should be used.

class NameStripper {
 public:
  bool StripName(const std::string& name) {
    if (name[name.size() - 1] != ')') {
      // (anonymous namespace)::ctype_w
      stripped_ = &name;
      return false;
    }

    int nesting = 0;
    for (size_t n = name.size() - 1; n < name.size(); --n) {
      if (name[n] == '(') {
        if (--nesting == 0) {
          storage_ = name.substr(0, n);
          stripped_ = &storage_;
          return true;
        }
      } else if (name[n] == ')') {
        ++nesting;
      }
    }


    stripped_ = &name;
    return false;
  }

  const std::string& stripped() { return *stripped_; }

 private:
  const std::string* stripped_;
  std::string storage_;
};


// Demangler ///////////////////////////////////////////////////////////////////

// Demangles C++ symbols.
//
// There is no library we can (easily) link against for this, we have to shell
// out to the "c++filt" program
//
// We can't use LineReader or popen() because we need to both read and write to
// the subprocess.  So we need to roll our own.

class Demangler {
 public:
  Demangler();
  ~Demangler();

  std::string Demangle(const std::string& symbol);

 private:
  FILE* write_file_;
  std::unique_ptr<LineReader> reader_;
  pid_t child_pid_;
};

Demangler::Demangler() {
  int toproc_pipe_fd[2];
  int fromproc_pipe_fd[2];
  if (pipe(toproc_pipe_fd) < 0 || pipe(fromproc_pipe_fd) < 0) {
    perror("pipe");
    exit(1);
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    exit(1);
  }

  if (pid) {
    // Parent.
    CHECK_SYSCALL(close(toproc_pipe_fd[0]));
    CHECK_SYSCALL(close(fromproc_pipe_fd[1]));
    int write_fd = toproc_pipe_fd[1];
    int read_fd = fromproc_pipe_fd[0];
    write_file_ = fdopen(write_fd, "w");
    FILE* read_file = fdopen(read_fd, "r");
    if (write_file_ == nullptr || read_file == nullptr) {
      perror("fdopen");
      exit(1);
    }
    reader_.reset(new LineReader(read_file, false));
    child_pid_ = pid;
  } else {
    // Child.
    CHECK_SYSCALL(close(STDIN_FILENO));
    CHECK_SYSCALL(close(STDOUT_FILENO));
    CHECK_SYSCALL(dup2(toproc_pipe_fd[0], STDIN_FILENO));
    CHECK_SYSCALL(dup2(fromproc_pipe_fd[1], STDOUT_FILENO));

    CHECK_SYSCALL(close(toproc_pipe_fd[0]));
    CHECK_SYSCALL(close(fromproc_pipe_fd[1]));
    CHECK_SYSCALL(close(toproc_pipe_fd[1]));
    CHECK_SYSCALL(close(fromproc_pipe_fd[0]));

    char prog[] = "c++filt";
    char *const argv[] = {prog, nullptr};
    CHECK_SYSCALL(execvp("c++filt", argv));
  }
}

Demangler::~Demangler() {
  int status;
  kill(child_pid_, SIGTERM);
  waitpid(child_pid_, &status, WEXITED);
  fclose(write_file_);
}

std::string Demangler::Demangle(const std::string& symbol) {
  const char *writeptr = symbol.c_str();
  const char *writeend = writeptr + symbol.size();

  while (writeptr < writeend) {
    size_t bytes = fwrite(writeptr, 1, writeend - writeptr, write_file_);
    if (bytes == 0) {
      perror("fread");
      exit(1);
    }
    writeptr += bytes;
  }
  if (fwrite("\n", 1, 1, write_file_) != 1) {
    perror("fwrite");
    exit(1);
  }
  if (fflush(write_file_) != 0) {
    perror("fflush");
    exit(1);
  }

  reader_->Next();
  return reader_->line();
}


// NameMunger //////////////////////////////////////////////////////////////////

// Use to transform input names according to the user's configuration.
// For example, the user can use regexes.
class NameMunger {
 public:
  // Adds a regex that will be applied to all names.  All regexes will be
  // applied in sequence.
  void AddRegex(const std::string& regex, const std::string& replacement);

  std::string Munge(const std::string& name) const;

 private:
  std::vector<std::pair<std::unique_ptr<RE2>, std::string>> regexes_;
};

void NameMunger::AddRegex(const std::string& regex, const std::string& replacement) {
  std::unique_ptr<RE2> re2(new RE2(re2::StringPiece(regex)));
  regexes_.push_back(std::make_pair(std::move(re2), replacement));
}

std::string NameMunger::Munge(const std::string& name) const {
  std::string ret = name;

  for (const auto& pair : regexes_) {
    if (RE2::Replace(&ret, *pair.first, pair.second)) {
      break;
    }
  }

  return ret;
}

struct ConfiguredDataSource {
  ConfiguredDataSource(const DataSource* source_) : source(source_) {}
  const DataSource* source;
  NameMunger munger;
};


// Rollup //////////////////////////////////////////////////////////////////////

// A Rollup is a hierarchical tally of sizes.  Its graphical representation is
// something like this:
//
//  93.3%  93.3%   3.02M Unmapped
//      38.2%  38.2%   1.16M .debug_info
//      23.9%  62.1%    740k .debug_str
//      12.1%  74.2%    374k .debug_pubnames
//      11.7%  86.0%    363k .debug_loc
//       8.9%  94.9%    275k [Other]
//       5.1% 100.0%    158k .debug_ranges
//   6.7% 100.0%    222k LOAD [R E]
//      61.0%  61.0%    135k .text
//      21.4%  82.3%   47.5k .rodata
//       6.2%  88.5%   13.8k .gcc_except_table
//       5.9%  94.4%   13.2k .eh_frame
//       5.6% 100.0%   12.4k [Other]
//   0.0% 100.0%   1.40k [Other]
// 100.0%   3.24M TOTAL
//
// There is a string -> size map of sizes (the meaning of the string labels
// depends on context; it can by symbols, sections, source filenames, etc.) Each
// map entry can have its own sub-Rollup.

std::string others_label = "[Other]";

class Rollup {
 public:
  Rollup() {}

  void AddSizes(const std::vector<std::string> names,
                uint64_t size, bool is_vmsize) {
    // We start at 1 to exclude the base map (see base_map_).
    AddInternal(names, 1, size, is_vmsize);
  }

  // Prints a graphical representation of the rollup.
  void PrintWithBase(int row_limit) const { PrintWithBase(nullptr, row_limit); }

  void PrintWithBase(Rollup* base, int row_limit) const {
    RollupRow row("TOTAL");
    row.vmsize = vm_total_;
    row.filesize = file_total_;
    row.vmpercent = 100;
    row.filepercent = 100;
    size_t longest_label = 0;
    ComputeRows(0, &row, &longest_label, base, base != nullptr, row_limit);
    row.PrintTree(0, longest_label, true);
  }

  // Subtract the values in "other" from this.
  void Subtract(const Rollup& other) {
    vm_total_ -= other.vm_total_;
    file_total_ -= other.file_total_;

    for (const auto& other_child : other.children_) {
      auto& child = children_[other_child.first];
      if (child.get() == NULL) {
        child.reset(new Rollup());
      }
      child->Subtract(*other_child.second);
    }
  }

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(Rollup);

  int64_t vm_total_ = 0;
  int64_t file_total_ = 0;

  // Putting Rollup by value seems to work on some compilers/libs but not
  // others.
  typedef std::unordered_map<std::string, std::unique_ptr<Rollup>> ChildMap;
  ChildMap children_;

  // Adds "size" bytes to the rollup under the label names[i].
  // If there are more entries names[i+1, i+2, etc] add them to sub-rollups.
  void AddInternal(const std::vector<std::string> names, size_t i,
                   int64_t size, bool is_vmsize) {
    if (is_vmsize) {
      vm_total_ += size;
    } else {
      file_total_ += size;
    }
    if (i < names.size()) {
      auto& child = children_[names[i]];
      if (child.get() == nullptr) {
        child.reset(new Rollup());
      }
      child->AddInternal(names, i + 1, size, is_vmsize);
    }
  }

  struct RollupRow {
    RollupRow(const std::string& name_) : name(name_) {}
    RollupRow(const ChildMap::value_type& value)
        : name(value.first),
          vmsize(value.second->vm_total_),
          filesize(value.second->file_total_) {}

    std::string name;
    int64_t vmsize = 0;
    int64_t filesize = 0;
    double vmpercent;
    double filepercent;
    bool has_base = false;
    std::vector<RollupRow> sorted_children;

    void Print(size_t indent, size_t longest_label) const;
    void PrintTree(size_t indent, size_t longest_label, bool is_base) const;
  };

  void ComputeRows(size_t indent, RollupRow* row, size_t* longest_label,
                   Rollup* base, bool has_base, int row_limit) const;
};

void Rollup::ComputeRows(size_t indent, RollupRow* row, size_t* longest_label,
                         Rollup* base, bool has_base, int row_limit) const {
  auto& child_rows = row->sorted_children;
  child_rows.reserve(children_.size());

  if (has_base) {
    row->vmpercent = Percent(vm_total_, base ? base->vm_total_ : 0);
    row->filepercent = Percent(file_total_, base ? base->file_total_ : 0);
    row->has_base = true;
  }

  if (children_.empty() ||
      (children_.size() == 1 &&
       children_.begin()->first == "[None]")) {
    return;
  }

  for (const auto& value : children_) {
    if (value.second->vm_total_ != 0 || value.second->file_total_ != 0) {
      child_rows.push_back(RollupRow(value));
    }
  }

  std::sort(child_rows.begin(), child_rows.end(),
            [](const RollupRow& a, const RollupRow& b) {
              return std::max(std::abs(a.vmsize), std::abs(a.filesize)) >
                     std::max(std::abs(b.vmsize), std::abs(b.filesize));
            });

  // Filter out everything but the top 'row_limit'.
  RollupRow others(others_label);

  size_t i = child_rows.size() - 1;
  while (i >= row_limit) {
    if (child_rows[i].name == "[None]") {
      // Don't collapse [None] into [Other].
      std::swap(child_rows[i], child_rows[19]);
    } else {
      others.vmsize += child_rows[i].vmsize;
      others.filesize += child_rows[i].filesize;
      child_rows.erase(child_rows.end() - 1);
      i--;
    }
  }

  if (others.vmsize > 0 || others.filesize > 0) {
    child_rows.push_back(others);
  }

  // Put [Other] in the right place.
  std::sort(child_rows.begin(), child_rows.end(), [](const RollupRow& a,
                                                     const RollupRow& b) {
    return std::max(a.vmsize, a.filesize) > std::max(b.vmsize, b.filesize);
  });

  // Compute percents for all rows (including "Other")
  if (!has_base) {
    for (auto& child_row : child_rows) {
      child_row.vmpercent = Percent(child_row.vmsize, row->vmsize);
      child_row.filepercent = Percent(child_row.filesize, row->filesize);
    }
  }

  // Recurse into sub-rows, (except "Other", which isn't a real row).
  Demangler demangler;
  NameStripper stripper;
  for (auto& child_row : child_rows) {
    if (child_row.name == others_label) {
      continue;
    }

    Rollup* child_base = nullptr;
    if (base) {
      child_base = base->children_[child_row.name].get();
    }

    auto it = children_.find(child_row.name);
    if (it == children_.end()) {
      std::cerr << "Should never happen, couldn't find name: "
                << child_row.name << "\n";
      exit(1);
    }

    auto demangled = demangler.Demangle(child_row.name);
    stripper.StripName(demangled);
    size_t allowed_label_len = std::min(stripper.stripped().size(), kMaxLabelLen);
    child_row.name = stripper.stripped();
    *longest_label = std::max(*longest_label, allowed_label_len + indent);
    it->second->ComputeRows(indent + 4, &child_row, longest_label, child_base,
                            has_base, row_limit);
  }
}

void Rollup::RollupRow::Print(size_t indent, size_t longest_label) const {
  std::cout << FixedWidthString("", indent) << " "
            << PercentString(vmpercent, has_base) << " "
            << SiPrint(vmsize, has_base) << " "
            << FixedWidthString(name, longest_label) << " "
            << SiPrint(filesize, has_base) << " "
            << PercentString(filepercent, has_base) << "\n";
}

void Rollup::RollupRow::PrintTree(size_t indent, size_t longest_label,
                                  bool is_base) const {
  NameStripper stripper;
  Demangler demangler;

  if (is_base) {
    printf("     VM SIZE    ");
    PrintSpaces(longest_label);
    printf("    FILE SIZE");
    printf("\n");
    printf(" -------------- ");
    PrintSpaces(longest_label);
    printf(" --------------");
    printf("\n");
  }

  if (!is_base) {
    Print(indent, longest_label);
  }

  for (const auto& child : sorted_children) {
    child.PrintTree(is_base ? indent : indent + 4, longest_label, false);
  }

  // The "TOTAL" row comes after all other rows.
  if (is_base) {
    Print(indent, longest_label);
  }
}

// RangeMap ////////////////////////////////////////////////////////////////////

// Maps
//
//   [uint64_t, uint64_t) -> std::string
//
// where ranges must be non-overlapping.
//
// This is used to map the address space (either pointer offsets or file
// offsets).

class RangeMap {
 public:
  bool AddRange(uint64_t addr, uint64_t size, const std::string& val);
  const std::string* Get(uint64_t addr, uint64_t* start, uint64_t* size) const;
  const std::string* TryGet(uint64_t addr, uint64_t* start,
                            uint64_t* size) const;
  const std::string* TryGetExactly(uint64_t addr, uint64_t* size) const;

  void Fill(uint64_t max, const std::string& val);

  static void ComputeRollup(const std::vector<const RangeMap*>& range_maps,
                            bool is_vmsize, const std::string& filename,
                            int filename_position, Rollup* rollup);

 private:
  typedef std::map<uint64_t, std::pair<std::string, uint64_t>> Map;
  Map mappings_;

  static uint64_t RangeEnd(Map::const_iterator iter) {
    return iter->first + iter->second.second;
  }

  bool IterIsEnd(Map::const_iterator iter) const {
    return iter == mappings_.end();
  }
};

const std::string* RangeMap::TryGet(uint64_t addr, uint64_t* start,
                                    uint64_t* size) const {
  auto it = mappings_.upper_bound(addr);
  if (it == mappings_.begin() ||
      (--it, it->first + it->second.second <= addr)) {
    return nullptr;
  }
  assert(addr >= it->first && addr < it->first + it->second.second);
  if (start) *start = it->first;
  if (size) *size = it->second.second;
  return &it->second.first;
}

const std::string* RangeMap::TryGetExactly(uint64_t addr, uint64_t* size) const {
  auto it = mappings_.find(addr);
  if (it == mappings_.end()) {
    return nullptr;
  }
  if (size) *size = it->second.second;
  return &it->second.first;
}

bool RangeMap::AddRange(uint64_t addr, uint64_t size, const std::string& val) {
  // XXX: properly test the whole range, not just the two endpoints.
  const std::string* existing;
  if ((existing = TryGet(addr, nullptr, nullptr)) != nullptr ||
      (existing = TryGet(addr + size - 1, nullptr, nullptr)) != nullptr) {
    if (*existing != val) {
      return false;
    }
  }
  mappings_[addr] = std::make_pair(std::move(val), size);
  return true;
}

void RangeMap::Fill(uint64_t max, const std::string& val) {
  uint64_t last = 0;

  for (const auto& pair : mappings_) {
    if (pair.first > last) {
      mappings_[last] = std::make_pair(val, pair.first - last);
    }

    last = pair.first + pair.second.second;
  }

  if (last < max) {
    mappings_[last] = std::make_pair(val, max - last);
  }
}

void RangeMap::ComputeRollup(const std::vector<const RangeMap*>& range_maps,
                             bool is_vmsize, const std::string& filename,
                             int filename_position, Rollup* rollup) {
  assert(range_maps.size() > 0);

  std::vector<Map::const_iterator> iters;
  std::vector<std::string> keys;
  uint64_t current = UINTPTR_MAX;

  for (auto range_map : range_maps) {
    iters.push_back(range_map->mappings_.begin());
    current = std::min(current, iters.back()->first);
  }

  assert(current != UINTPTR_MAX);

  // Iterate over all ranges in parallel to perform this transformation:
  //
  //   -----  -----  -----             ---------------
  //     |      |      1                    A,X,1
  //     |      X    -----             ---------------
  //     |      |      |                    A,X,2
  //     A    -----    |               ---------------
  //     |      |      |                      |
  //     |      |      2      ----->          |
  //     |      Y      |                    A,Y,2
  //     |      |      |                      |
  //   -----    |      |               ---------------
  //     B      |      |                    B,Y,2
  //   -----    |    -----             ---------------
  //            |                      [None],Y,[None]
  //          -----
  while (true) {
    uint64_t next_break = UINTPTR_MAX;
    bool have_data = false;
    keys.clear();
    size_t i;

    for (i = 0; i < iters.size(); i++) {
      auto& iter = iters[i];

      if (filename_position >= 0 &&
          static_cast<unsigned>(filename_position) == i) {
        keys.push_back(filename);
      }

      // Advance the iterators if its range is behind the current point.
      while (!range_maps[i]->IterIsEnd(iter) && RangeEnd(iter) <= current) {
        ++iter;
        //assert(range_maps[i]->IterIsEnd(iter) || RangeEnd(iter) > current);
      }

      // Push a label and help calculate the next break.
      bool is_end = range_maps[i]->IterIsEnd(iter);
      if (is_end || iter->first > current) {
        keys.push_back("[None]");
        if (!is_end) {
          next_break = std::min(next_break, iter->first);
        }
      } else {
        have_data = true;
        keys.push_back(iter->second.first);
        next_break = std::min(next_break, RangeEnd(iter));
      }
    }

    if (filename_position >= 0 &&
        static_cast<unsigned>(filename_position) == i) {
      keys.push_back(filename);
    }

    if (next_break == UINTPTR_MAX) {
      break;
    }

    if (false) {
      for (auto& key : keys) {
        if (key == "[None]") {
          std::stringstream stream;
          stream << " [0x" << std::hex << current << ", 0x" << std::hex
                 << next_break << "]";
          key += stream.str();
        }
      }
    }

    if (have_data) {
      rollup->AddSizes(keys, next_break - current, is_vmsize);
    }

    current = next_break;
  }
}


// MemoryMap ///////////////////////////////////////////////////////////////////

// Contains a [range] -> label map for VM space and file space.
class MemoryMap {
 public:
  MemoryMap(NameMunger* munger) : munger_(munger) {}
  virtual ~MemoryMap() {}

  bool FindAtAddr(uint64_t vmaddr, std::string* name) const;
  bool FindContainingAddr(uint64_t vmaddr, uint64_t* start,
                          std::string* name) const;

  const RangeMap* file_map() const { return &file_map_; }
  const RangeMap* vm_map() const { return &vm_map_; }
  RangeMap* file_map() { return &file_map_; }
  RangeMap* vm_map() { return &vm_map_; }

 protected:
  std::string ApplyNameRegexes(const std::string& name);

 private:
  BLOATY_DISALLOW_COPY_AND_ASSIGN(MemoryMap);
  friend class VMRangeSink;

  RangeMap vm_map_;
  RangeMap file_map_;
  const NameMunger* munger_ = nullptr;
  std::unordered_map<std::string, std::string> aliases_;
};

std::string MemoryMap::ApplyNameRegexes(const std::string& name) {
  return munger_ ? munger_->Munge(name) : name;
}


// MemoryFileMap ///////////////////////////////////////////////////////////////

// A MemoryMap for things like segments and sections, where every range exists
// in both VM space and file space.  We can use MemoryFileMaps to translate VM
// addresses into file offsets.
class MemoryFileMap : public MemoryMap {
 public:
  MemoryFileMap(NameMunger* munger) : MemoryMap(munger) {}
  virtual ~MemoryFileMap() {}

  void AddRange(const std::string& name, uint64_t vmaddr, uint64_t vmsize,
                long fileoff, long filesize);

  // Translates a VM address to a file offset.  Returns false if this VM address
  // is not mapped from the file.
  bool TranslateVMAddress(uint64_t vmaddr, uint64_t* fileoff) const;

  // Translates a VM address range to a file offset range.  Returns false if
  // nothing in this VM address range is mapped into a file.
  //
  // This VM address range may not translate to multiple discrete file ranges.
  // We generally would never expect this to happen.
  bool TranslateVMRange(uint64_t vmaddr, uint64_t vmsize,
                        uint64_t* fileoff, uint64_t* filesize) const;

 private:
  // Maps each vm_map_ start address to a corresponding file_map_ start address.
  // If a given vm_map_ start address is missing from the map, it does not come
  // from the file.
  std::unordered_map<uint64_t, uint64_t> vm_to_file_;
};

void MemoryFileMap::AddRange(const std::string& name,
                             uint64_t vmaddr, uint64_t vmsize,
                             long fileoff, long filesize) {
  std::string label = ApplyNameRegexes(name);
  if ((vmsize > 0 && !vm_map()->AddRange(vmaddr, vmsize, label)) ||
      (filesize > 0 && !file_map()->AddRange(fileoff, filesize, label))) {
    if (warnings) {
      std::cerr << "bloaty: unexpected overlap adding name '" << name << "'\n";
    }
    return;
  }

  if (vmsize > 0 && filesize > 0) {
    vm_to_file_[vmaddr] = fileoff;
  }
}

bool MemoryFileMap::TranslateVMAddress(uint64_t vmaddr,
                                       uint64_t* fileoff) const {
  uint64_t vmstart;
  if (!vm_map()->TryGet(vmaddr, &vmstart, nullptr)) {
    return false;
  }

  auto it = vm_to_file_.find(vmstart);
  if (it == vm_to_file_.end()) {
    return false;
  }

  uint64_t filestart = it->second;
  uint64_t filesize;
  if (!file_map()->TryGetExactly(filestart, &filesize)) {
    std::cerr << "Fatal error, should never happen.\n";
    exit(1);
  }

  if (vmaddr - vmstart > filesize) {
    // File mapping is shorter than VM mapping and doesn't actually contain our
    // address.
    return false;
  }

  *fileoff = (vmaddr - vmstart) + filestart;
  return true;
}

bool MemoryFileMap::TranslateVMRange(uint64_t vmaddr, uint64_t vmsize,
                                     uint64_t* fileoff,
                                     uint64_t* filesize) const {
  uint64_t vm_range_start, vm_range_size;
  if (!vm_map()->TryGet(vmaddr, &vm_range_start, &vm_range_size)) {
    if (warnings) {
      std::cerr << "Address: " << vmaddr << " wasn't in VM map\n";
    }
    return false;
  }

  if (vmaddr + vmsize > vm_range_start + vm_range_size) {
    std::cerr << "Tried to translate range that spanned regions of of our "
              << "mapping.  This shouldn't happen.\n";
    std::cerr << "Translating range: [" << vmaddr << ", " << (vmaddr + vmsize)
              << "\n";
    std::cerr << "Found mapping: {" << vm_range_start << ", "
              << vm_range_start + vm_range_size << "\n";
    exit(1);
  }

  auto it = vm_to_file_.find(vm_range_start);
  if (it == vm_to_file_.end()) {
    return false;
  }

  uint64_t file_range_start = it->second;
  uint64_t file_range_size;
  if (!file_map()->TryGetExactly(file_range_start, &file_range_size)) {
    std::cerr << "Fatal error, should never happen.\n";
    exit(1);
  }

  if (vmaddr - vm_range_start > file_range_size) {
    // File mapping is shorter than VM mapping and doesn't actually contain our
    // address.
    std::cerr << "File mapping is too short\n";
    return false;
  }

  *fileoff = (vmaddr - vm_range_start) + file_range_start;
  *filesize = std::min(vmsize, file_range_size - (vmsize - vm_range_start));
  return true;
}


// InputFile ///////////////////////////////////////////////////////////////////

// Contains a set of MemoryMap/MemoryFileMap objects for a given input file.
// We have one of these objects per input file.  Each one of these objects
// contains a map for every data source that the user selected.
//
// Note that in the case of an archive input file (.a), we create these classes
// for the files inside (the .o files).  We do NOT have an InputFile object for
// the .a itself.

class InputFile {
 public:
  InputFile(const std::string& filename, uint64_t size, Platform* platform);

  void PushMapFor(ConfiguredDataSource* source);

  // Compute rollups for this file and add them to "rollup".  If
  // "filename_position" is >= 0, that indicates where in the rollup the
  // filename should be inserted.
  void ComputeRollup(int filename_position, Rollup* rollup);

  MemoryMap* GetLastMap() {
    return maps_.empty() ? &base_map_ : maps_.back().get();
  }

  const MemoryFileMap& GetBaseMap() { return base_map_; }

 private:
  void PushMap(const MemoryMap& map) {
    vm_maps_.push_back(map.vm_map());
    file_maps_.push_back(map.file_map());
  }

  const std::string filename_;
  uint64_t size_;
  Platform* platform_;
  MemoryFileMap base_map_;
  std::vector<std::unique_ptr<MemoryMap>> maps_;

  std::vector<const RangeMap*> vm_maps_;
  std::vector<const RangeMap*> file_maps_;
};

InputFile::InputFile(const std::string& filename, uint64_t size,
                     Platform* platform)
    : filename_(filename),
      size_(size),
      platform_(platform),
      base_map_(nullptr) {
  // Start with the base map, so that any unaccounted space is shown as [None]
  // instead of just being missing from the totals.  The rollup will discard
  // this so it's not actually visible.
  PushMap(base_map_);
}

void InputFile::PushMapFor(ConfiguredDataSource* source) {
  switch (source->source->type) {
    case DataSource::DATA_SOURCE_VM_RANGE: {
      std::unique_ptr<MemoryMap> map(new MemoryMap(&source->munger));
      maps_.push_back(std::move(map));
      break;
    }
    case DataSource::DATA_SOURCE_VM_FILE_RANGE: {
      std::unique_ptr<MemoryFileMap> map(new MemoryFileMap(&source->munger));
      maps_.push_back(std::move(map));
      break;
    }
  }

  PushMap(*maps_.back());
}

void InputFile::ComputeRollup(int filename_position, Rollup* rollup) {
  // How that the base map has been set, fill out the file size to ensure that
  // we cover the entire file.
  base_map_.file_map()->Fill(size_, "[Unmapped]");
  RangeMap::ComputeRollup(vm_maps_, true, filename_, filename_position, rollup);
  RangeMap::ComputeRollup(file_maps_, false, filename_, filename_position,
                          rollup);
}


// InputFileMap ////////////////////////////////////////////////////////////////

// A [filename -> InputFile] map and code to lazily initialize map entries.
//
// Usually this map will have just a single entry in it, for the primary
// filename.  We only need the map functionality for archive files, which have
// several actual object files inside.

class InputFileMap {
 public:
  InputFileMap(const std::string& main_filename, Platform* platform)
      : main_filename_(main_filename), platform_(platform) {
    // Scan base maps; this stage discovers all files.
    VMFileRangeSink sink(this);
    platform_->AddBaseMap(main_filename, &sink);
  }

  // Scan maps for the given data source.
  void ScanDataSource(ConfiguredDataSource* source);

  // Adds a file to the map with the given size.  This must be called before
  // GetFile() is called for this filename.  Also, *all* AddFile() calls must
  // come before any GetFile() calls.
  void AddFile(const std::string& filename, uint64_t size);

  // Gets an InputFile that was previous called with AddFile().
  InputFile* GetFile(const std::string& filename);

  // Compute rollup and add to the given rollup object.
  void ComputeRollup(int filename_position, Rollup* rollup);

 private:
  const std::string main_filename_;
  Platform* platform_;
  ConfiguredDataSource* current_source_;
  std::unordered_map<std::string, std::unique_ptr<InputFile>> files_;
};

InputFile* InputFileMap::GetFile(const std::string& filename) {
  auto& file = files_[filename];

  if (!file.get()) {
    fprintf(stderr,
            "bloaty: Data source found file inside archive that initial scan "
            "did not find: %s\n", filename.c_str());
    exit(1);
  }

  return file.get();
}

void InputFileMap::AddFile(const std::string& filename, uint64_t size) {
  auto& file = files_[filename];

  if (file.get()) {
    fprintf(stderr, "bloaty: File was found twice in initial scan: %s\n",
            filename.c_str());
    exit(1);
  }

  file.reset(new InputFile(filename, size, platform_));
}

void InputFileMap::ScanDataSource(ConfiguredDataSource* source) {
  for (const auto& pair : files_) {
    pair.second->PushMapFor(source);
  }

  current_source_ = source;
  switch (source->source->type) {
    case DataSource::DATA_SOURCE_VM_RANGE: {
      VMRangeSink sink(this);
      source->source->func.vm_range(main_filename_, &sink);
      break;
    }

    case DataSource::DATA_SOURCE_VM_FILE_RANGE: {
      VMFileRangeSink sink(this);
      source->source->func.vm_file_range(main_filename_, &sink);
      break;
    }

    default:
      std::cerr << "bloaty: unknown source type?\n";
      exit(1);
  }
}

void InputFileMap::ComputeRollup(int filename_position, Rollup* rollup) {
  for (const auto& pair : files_) {
    pair.second->ComputeRollup(filename_position, rollup);
  }
}


// VMRangeSink ////////////////////////////////////////////////////////////////

void VMRangeSink::SetFilename(const std::string& filename) {
  auto file = input_files_->GetFile(filename);
  map_ = file->GetLastMap();
  translator_ = &file->GetBaseMap();
}

// Adds a region to the memory map.  It should not overlap any previous
// region added with Add(), but it should overlap the base memory map.
void VMRangeSink::AddVMRange(uint64_t vmaddr, uint64_t vmsize,
                             const std::string& name) {
  assert(map_);
  if (vmsize == 0) {
    return;
  }
  const std::string label = map_->ApplyNameRegexes(name);
  uint64_t fileoff, filesize;
  if (!map_->vm_map()->AddRange(vmaddr, vmsize, label) && warnings) {
    std::cerr << "Error adding range to VM map for name: " << name << "=["
              << std::hex << vmaddr << ", " << std::hex << vmsize << "]\n";
    uint64_t vmstart, vm_region_size;
    auto existing = map_->vm_map()->TryGet(vmaddr, &vmstart, &vm_region_size);
    if (!existing) {
      existing = map_->vm_map()->TryGet(vmaddr + vmsize - 1, &vmstart, &vm_region_size);
    }
    std::cerr << "Existing mapping: " << *existing << "=[" << vmstart << ", "
              << vm_region_size << "]\n";
  }
  if (translator_->TranslateVMRange(vmaddr, vmsize, &fileoff, &filesize)) {
    if (!map_->file_map()->AddRange(fileoff, filesize, label) && warnings) {
      std::cerr << "Error adding range to file map for name: " << name << "\n";
    }
  }
}

void VMRangeSink::AddVMRangeAllowAlias(uint64_t vmaddr, uint64_t size,
                                       const std::string& name) {
  assert(map_);
  uint64_t existing_size;
  const auto existing = map_->vm_map()->TryGetExactly(vmaddr, &existing_size);
  if (existing) {
    if (size != existing_size) {
      if (warnings) {
        std::cerr << "bloaty: Warning: inexact alias for name: " << name
                  << ", aliases=" << *existing << "\n";
      }
    }
    map_->aliases_.insert(std::make_pair(name, *existing));
  } else {
    AddVMRange(vmaddr, size, name);
  }
}

void VMRangeSink::AddVMRangeIgnoreDuplicate(uint64_t vmaddr, uint64_t vmsize,
                                            const std::string& name) {
  assert(map_);
  if (vmsize == 0) {
    return;
  }

  const std::string label = map_->ApplyNameRegexes(name);
  uint64_t fileoff, filesize;
  map_->vm_map()->AddRange(vmaddr, vmsize, label);
  if (translator_->TranslateVMRange(vmaddr, vmsize, &fileoff, &filesize)) {
    map_->file_map()->AddRange(fileoff, filesize, label);
  }
}


// VMFileRangeSink /////////////////////////////////////////////////////////////

void VMFileRangeSink::AddFile(const std::string& filename, uint64_t size) {
  input_files_->AddFile(filename, size);
}

void VMFileRangeSink::SetFilename(const std::string& filename) {
  auto file = input_files_->GetFile(filename);
  map_ = down_cast<MemoryFileMap*>(file->GetLastMap());
}

void VMFileRangeSink::AddRange(const std::string& name, uint64_t vmaddr,
                               uint64_t vmsize, uint64_t fileoff,
                               uint64_t filesize) {
  assert(map_);
  map_->AddRange(name, vmaddr, vmsize, fileoff, filesize);
}


// Bloaty //////////////////////////////////////////////////////////////////////

// Represents a program execution and associated state.

class Bloaty {
 public:
  Bloaty();

  void SetFilename(const std::string& filename);
  void SetBaseFile(const std::string& filename);

  void AddDataSource(const std::string& name);
  ConfiguredDataSource* FindDataSource(const std::string& name) const;
  size_t GetSourceCount() const {
    return sources_.size() + (filename_position_ >= 0 ? 1 : 0);
  }

  void ScanAndRollup(Rollup* rollup);
  void PrintDataSources() const {
    for (const auto& source : all_known_sources_) {
      fprintf(stderr, "%s\n", source.first.c_str());
    }
  }

  void SetRowLimit(int n) { row_limit_ = (n == 0) ? INT_MAX : n; }

 private:
  void ScanAndRollupFile(const std::string& filename, Rollup* rollup);
  static void CheckFileReadable(const std::string& filename);
  static long GetFileSize(const std::string& filename);

  std::unique_ptr<Platform> platform_;
  std::map<std::string, DataSource> all_known_sources_;
  std::vector<ConfiguredDataSource> sources_;
  std::map<std::string, ConfiguredDataSource*> sources_by_name_;
  std::string filename_;
  std::string base_filename_;
  int row_limit_;
  int filename_position_;
};

void Bloaty::CheckFileReadable(const std::string& filename) {
  FILE* file = fopen(filename.c_str(), "rb");
  if (!file) {
    std::cerr << "Couldn't open file for reading: " << filename << "\n";
    exit(1);
  }
  fclose(file);
}

Bloaty::Bloaty() : row_limit_(20), filename_position_(-1) {
#ifdef __APPLE__
  platform_ = NewMachOPlatformModule();
#else
  platform_ = NewELFPlatformModule();
#endif

  std::vector<DataSource> all_sources;
  platform_->RegisterDataSources(&all_sources);

  for (const auto& source : all_sources) {
    if (!all_known_sources_.insert(std::make_pair(source.name, source))
             .second) {
      std::cerr << "Two data sources registered for name: "
                << source.name << "\n";
      exit(1);
    }
  }
}

void Bloaty::SetFilename(const std::string& filename) {
  if (!filename_.empty()) {
    std::cerr << "Only one filename can be specified.\n";
    exit(1);
  }
  CheckFileReadable(filename);
  filename_ = filename;
}

void Bloaty::SetBaseFile(const std::string& filename) {
  if (!base_filename_.empty()) {
    std::cerr << "Only one base filename can be specified.\n";
    exit(1);
  }
  CheckFileReadable(filename);
  base_filename_ = filename;
}

void Bloaty::AddDataSource(const std::string& name) {
  if (name == "inputfiles") {
    filename_position_ = sources_.size() + 1;
    return;
  }

  auto it = all_known_sources_.find(name);
  if (it == all_known_sources_.end()) {
    std::cerr << "bloaty: no such data source: " << name << "\n";
    exit(1);
  }

  sources_.emplace_back(&it->second);
  sources_by_name_[name] = &sources_.back();
}

ConfiguredDataSource* Bloaty::FindDataSource(const std::string& name) const {
  auto it = sources_by_name_.find(name);
  if (it != sources_by_name_.end()) {
    return it->second;
  } else {
    return NULL;
  }
}

void Bloaty::ScanAndRollupFile(const std::string& filename, Rollup* rollup) {
  InputFileMap map(filename, platform_.get());

  for (size_t i = 0; i < sources_.size(); i++) {
    map.ScanDataSource(&sources_[i]);
  }

  map.ComputeRollup(filename_position_, rollup);
}

void Bloaty::ScanAndRollup(Rollup* rollup) {
  if (filename_.empty()) {
    fputs("bloaty: no filename specified, exiting.\n", stderr);
    exit(1);
  }

  ScanAndRollupFile(filename_, rollup);

  if (!base_filename_.empty()) {
    Rollup base;
    ScanAndRollupFile(base_filename_, &base);
    rollup->Subtract(base);
    rollup->PrintWithBase(&base, row_limit_);
  } else {
    rollup->PrintWithBase(nullptr, row_limit_);
  }
}

#if 0
class Program {
 public:
  void CalculatePrettyNames() {
    if (pretty_names_calculated_) {
      return;
    }

    for (auto& pair: objects_) {
      Object* obj = pair.second;
      auto demangled = demangler_.Demangle(obj->name);
      if (stripper_.StripName(demangled)) {
        auto it = stripped_pretty_names_.find(stripper_.stripped());
        if (it == stripped_pretty_names_.end()) {
          stripped_pretty_names_[stripper_.stripped()] = obj;
          obj->pretty_name = stripper_.stripped();
        } else {
          obj->pretty_name = demangled;
          if (it->second) {
            it->second->pretty_name = demangler_.Demangle(it->second->name);
            it->second = NULL;
          }
        }
      } else {
        obj->pretty_name = demangled;
      }
    }

    pretty_names_calculated_ = true;
  }
#endif


}  // namespace bloaty

using namespace bloaty;

const char usage[] = R"(Bloaty McBloatface: a size profiler for binaries.

USAGE: bloaty [options] <binary>

Options:

  -b <binary>      Show a diff view, with <binary> as the base.
  -d <sources>     Comma-separated list of sources to scan.
  -r <regex>       Add regex to the list of regexes.
                   Format for regex is:
                     SOURCE=~/PATTERN/REPLACEMENT/
  -n <num>         How many rows to show per level before collapsing
                   other keys into '[Other]'.  Set to '0' for unlimited.
                   Defaults to 20.
  -W               Show warnings.  Use when developing/debugging data
                   sources or when you distrust the results.
  --help           Display this message and exit.
  --list-sources   Show a list of available sources and exit.
)";

void CheckNextArg(int i, int argc, const char *option) {
  if (i + 1 >= argc) {
    fprintf(stderr, "bloaty: option '%s' requires an argument\n", option);
    exit(1);
  }
}

int main(int argc, char *argv[]) {
  bloaty::Bloaty bloaty;

  RE2 regex_pattern("(\\w+)\\+=/(.*)/(.*)/");

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-b") == 0) {
      bloaty.SetBaseFile(argv[++i]);
    } else if (strcmp(argv[i], "-d") == 0) {
      CheckNextArg(i, argc, "-d");
      std::vector<std::string> names;
      Split(argv[++i], ',', &names);
      for (const auto& name : names) {
        bloaty.AddDataSource(name);
      }
    } else if (strcmp(argv[i], "-r") == 0) {
      std::string source_name, regex, substitution;
      if (!RE2::FullMatch(argv[++i], regex_pattern,
                          &source_name, &regex, &substitution)) {
        std::cerr << "Bad format for regex, should be: "
                  << "source=~/pattern/replacement/\n";
        exit(1);
      }

      auto source = bloaty.FindDataSource(source_name);
      if (!source) {
        std::cerr << "Data source '" << source_name
                  << "' not found in previous "
                  << "-d option\n";
        exit(1);
      }

      source->munger.AddRegex(regex, substitution);
    } else if (strcmp(argv[i], "-n") == 0) {
      CheckNextArg(i, argc, "-n");
      bloaty.SetRowLimit(strtod(argv[++i], NULL));
    } else if (strcmp(argv[i], "-W") == 0) {
      warnings = true;
    } else if (strcmp(argv[i], "--list-sources") == 0) {
      bloaty.PrintDataSources();
      exit(1);
    } else if (strcmp(argv[i], "--help") == 0) {
      fputs(usage, stderr);
      exit(1);
    } else if (argv[i][0] == '-') {
      std::cerr << "Unknown option: " << argv[i] << "\n";
      exit(1);
    } else {
      bloaty.SetFilename(argv[i]);
    }
  }

  if (bloaty.GetSourceCount() == 0) {
    // Default when no sources are specified.
    bloaty.AddDataSource("sections");
  }

  Rollup rollup;
  bloaty.ScanAndRollup(&rollup);
  return 0;
}

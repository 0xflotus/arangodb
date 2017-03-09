////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_PREGEL_MMAP_H
#define ARANGODB_PREGEL_MMAP_H 1

#include <cstddef>
#include "Basics/Common.h"

namespace arangodb {
namespace pregel {

/// Portable read-only memory mapping (Windows and Linux)
/** Filesize limited by size_t, usually 2^32 or 2^64 */
class MemoryMapped {
 protected:
  /// open file, mappedBytes = 0 maps the whole file
  MemoryMapped(const std::string& filename, size_t mappedBytes,
                CacheHint hint);

 public:
  /// close file (see close() )
  ~MemoryMapped();

  /// open file, mappedBytes = 0 maps the whole file
  static MemoryMapped* openHelper(std::string const& filename, bool ignoreErrors);

  /// @brief return whether the datafile is a physical file (true) or an
  /// anonymous mapped region (false)
  inline bool isPhysical() const { return !_filename.empty(); }

  /// close file
  void close();

  /// access position, no range checking (faster)
  unsigned char operator[](size_t offset) const;
  /// access position, including range checking
  unsigned char at(size_t offset) const;

  /// raw access
  const unsigned char* getData() const;

  /// true, if file successfully opened
  bool isValid() const;

  /// get file size
  uint64_t size() const;
  /// get number of actually mapped bytes
  size_t mappedSize() const;

  /// replace mapping by a new one of the same file, offset MUST be a multiple
  /// of the page size
  bool remap(uint64_t offset, size_t mappedBytes);

 private:
  /// don't copy object
  MemoryMapped(const MemoryMapped&);
  /// don't copy object
  MemoryMapped& operator=(const MemoryMapped&);

  /// get OS page size (for remap)
  static int getpagesize();

  std::string _filename;     // underlying filename
  int _fd;                   // underlying file descriptor
#ifdef _MSC_VER
  void* _mmHandle;  // underlying memory map object handle (windows only)
#endif

  
  char* _data;  // start of the data array
  char* _next;  // end of the current data
};
}
}

#endif

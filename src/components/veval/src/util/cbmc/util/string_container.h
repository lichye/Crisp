/*******************************************************************\

Module: Container for C-Strings

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

/// \file
/// Container for C-Strings

#ifndef CPROVER_UTIL_STRING_CONTAINER_H
#define CPROVER_UTIL_STRING_CONTAINER_H

#include "memory_units.h"
#include "string_hash.h"

#include <list>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <array>
#include <atomic>
#include <shared_mutex>

// NOLINTNEXTLINE(readability/identifiers)
class string_view_hasht
{
public:
  std::size_t operator()(std::string_view s) const
  {
    return hash_string(s);
  }
};

/// Has estimated statistics about string container
/// (estimated because this only uses public information,
///  which disregards any internal control structures that
///  might be in use)
struct string_container_statisticst
{
  std::size_t string_count;
  memory_sizet strings_memory_usage;
  memory_sizet vector_memory_usage;
  memory_sizet map_memory_usage;
  memory_sizet list_memory_usage;

  void dump_on_stream(std::ostream &out) const;
};

class string_containert
{
public:
  unsigned operator[](std::string_view s)
  {
    return get(s);
  }

  // constructor and destructor
  string_containert();
  ~string_containert();

  // the pointer is guaranteed to be stable
  const char *c_str(size_t no) const
  {
    return get_string(no).c_str();
  }

  // the reference is guaranteed to be stable
  const std::string &get_string(size_t no) const
  {
    return *segments[no >> segment_bits].load(std::memory_order_acquire)
      ->strings[no & (segment_size - 1)].load(std::memory_order_acquire);
  }

  string_container_statisticst compute_statistics() const;

protected:
  // The keys are std::string_views into the std::string objects owned by
  // string_list below; std::list keeps those addresses stable and the interned
  // strings are never mutated, so the keys remain valid for the lifetime of
  // the map.
  // the 'unsigned' ought to be size_t
  typedef std::unordered_map<std::string_view, unsigned, string_view_hasht>
    hash_tablet;
  hash_tablet hash_table;

  unsigned get(std::string_view);

  typedef std::list<std::string> string_listt;
  string_listt string_list;

  typedef std::vector<std::string *> string_vectort;
  string_vectort string_vector;

  // Interned strings never move or change. Segments let readers use existing
  // IDs without locking while another worker interns a new name.
  static constexpr unsigned segment_bits = 16;
  static constexpr size_t segment_size = size_t{1} << segment_bits;
  struct segmentt {
    std::array<std::atomic<const std::string *>, segment_size> strings{};
  };
  std::array<std::atomic<segmentt *>, segment_size> segments{};
  mutable std::shared_mutex mutex;
};

/// Get a reference to the global string container.
inline string_containert &get_string_container()
{
  static string_containert ret;
  return ret;
}

#endif // CPROVER_UTIL_STRING_CONTAINER_H

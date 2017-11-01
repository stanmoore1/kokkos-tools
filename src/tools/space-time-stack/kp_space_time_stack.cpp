#include <cstdint>
#include <iostream>
#include <ios>
#include <iomanip>
#include <cstdlib>
#include <memory>
#include <string>
#include <set>
#include <cassert>
#include <queue>
#include <sys/resource.h>

#define USE_MPI

#ifdef USE_MPI
#include <mpi.h>
#endif

#include <chrono>

namespace {

struct KokkosPDeviceInfo {
  std::uint32_t deviceID;
};

struct SpaceHandle {
  char name[64];                                                                        
};

enum Space {
  SPACE_HOST,
  SPACE_CUDA
};

enum { NSPACES = 2 };

Space get_space(SpaceHandle const& handle) {
  switch (handle.name[0]) {
    case 'H': return SPACE_HOST;
    case 'C': return SPACE_CUDA;
  }
  abort();
  return SPACE_HOST;
}

const char* get_space_name(int space) {
  switch (space) {
    case SPACE_HOST: return "HOST";
    case SPACE_CUDA: return "CUDA";
  }
  abort();
  return nullptr;
}

struct Now {
  typedef std::chrono::time_point<std::chrono::high_resolution_clock> Impl;
  Impl impl;
};

Now now() {
  Now t;
  t.impl = std::chrono::high_resolution_clock::now();
  return t;
}

double operator-(Now b, Now a) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(b.impl - a.impl)
             .count() *
         1e-9;
}

enum StackKind {
  STACK_FOR,
  STACK_REDUCE,
  STACK_SCAN,
  STACK_REGION,
  STACK_COPY
};

void print_process_hwm() {
  struct rusage sys_resources;
  getrusage(RUSAGE_SELF, &sys_resources);
  long hwm = sys_resources.ru_maxrss;
  long hwm_max = hwm;

#ifdef USE_MPI
  int rank, world_size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);

  // Max
  MPI_Reduce(&hwm, &hwm_max, 1, MPI_LONG, MPI_MAX, 0, MPI_COMM_WORLD);

  // Min
  long hwm_min;
  MPI_Reduce(&hwm, &hwm_min, 1, MPI_LONG, MPI_MIN, 0, MPI_COMM_WORLD);

  // Average
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);
  long hwm_ave;
  MPI_Reduce(&hwm, &hwm_ave, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
  hwm_ave /= world_size;

  if (rank == 0)
#endif
  {
    printf("Host process high water mark memory consumption: %ld kB\n",
      hwm_max);
#ifdef USE_MPI
    printf("  Max: %ld, Min: %ld, Ave: %ld kB\n",
      hwm_max,hwm_min,hwm_ave);
#endif
    printf("\n");
  }
}

struct StackNode {
  StackNode* parent;
  std::string name;
  StackKind kind;
  std::set<StackNode> children;
  double total_runtime;
  double max_runtime;
  double avg_runtime;
  std::int64_t number_of_calls;
  Now start_time;
  StackNode(StackNode* parent_in, std::string&& name_in, StackKind kind_in):
    parent(parent_in),
    name(std::move(name_in)),
    kind(kind_in),
    total_runtime(0.0),
    number_of_calls(0) {
  }
  StackNode* get_child(std::string&& child_name, StackKind child_kind) {
    StackNode candidate(this, std::move(child_name), child_kind);
    auto it = children.find(candidate);
    if (it == children.end()) {
      auto res = children.emplace(std::move(candidate));
      it = res.first;
      assert(res.second);
    }
    return const_cast<StackNode*>(&(*(it)));
  }
  bool operator<(StackNode const& other) const {
    if (kind != other.kind) {
      return int(kind) < int(other.kind);
    }
    return name < other.name;
  }
  std::string get_full_name() const {
    std::string full_name;
    for (auto p = this; p; p = p->parent) {
      if (p->name.empty() && !p->parent) continue;
      full_name = p->name + '/' + full_name;
    }
    return full_name;
  }
  void begin() {
    number_of_calls++;
    start_time = now();
  }
  void end(Now const& end_time) {
    auto runtime = (end_time - start_time);
    total_runtime += runtime;
  }
  StackNode invert() const {
    StackNode inv_root(nullptr, "", STACK_REGION);
    std::queue<StackNode const*> q;
    q.push(this);
    while (!q.empty()) {
      auto node = q.front(); q.pop();
      auto self_time = node->total_runtime;
      auto calls = node->number_of_calls;
      for (auto& child : node->children) {
        self_time -= child.total_runtime;
        q.push(&child);
      }
      auto inv_node = &inv_root;
      inv_node->total_runtime += self_time;
      inv_node->number_of_calls += calls;
      for (; node; node = node->parent) {
        std::string name = node->name;
        inv_node = inv_node->get_child(std::move(name), node->kind);
        inv_node->total_runtime += self_time;
        inv_node->number_of_calls += calls;
      }
    }
    return inv_root;
  }
  void print_recursive(
      std::ostream& os, std::string my_indent, std::string const& child_indent, double tree_time) const {
    auto percent = (total_runtime / tree_time) * 100.0;
    if (percent < 0.1) return;
    if (!name.empty()) {
      os << my_indent;
      auto imbalance = (max_runtime / avg_runtime - 1.0) * 100.0;
      os << percent << "% " << imbalance << "% " << number_of_calls << " " << name;
      switch (kind) {
        case STACK_FOR: os << " [for]"; break;
        case STACK_REDUCE: os << " [reduce]"; break;
        case STACK_SCAN: os << " [scan]"; break;
        case STACK_REGION: os << " [region]"; break;
        case STACK_COPY: os << " [copy]"; break;
      };
      os << '\n';
    }
    if (children.empty()) return;
    auto by_time = [](StackNode const* a, StackNode const* b) {
      if (a->total_runtime != b->total_runtime) {
        return a->total_runtime > b->total_runtime;
      }
      return a->name < b->name;
    };
    std::set<StackNode const*, decltype(by_time)> children_by_time(by_time);
    for (auto& child : children) {
      children_by_time.insert(&child);
    }
    auto last = children_by_time.end();
    --last;
    for (auto it = children_by_time.begin(); it != children_by_time.end(); ++it) {
      std::string grandchild_indent;
      if (it == last) {
        grandchild_indent = child_indent + "    ";
      } else {
        grandchild_indent = child_indent + "|   ";
      }
      auto child = *it;
      child->print_recursive(
          os, child_indent + "|-> ", grandchild_indent, tree_time);
    }
  }
  void print(std::ostream& os) const {
    std::ios saved_state(nullptr);
    saved_state.copyfmt(os);
    os << std::fixed << std::setprecision(1);
    print_recursive(os, "", "", total_runtime);
    os << '\n';
    os.copyfmt(saved_state);
  }
  void reduce_over_mpi() {
#ifdef USE_MPI
    int rank, comm_size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &comm_size);
    std::queue<StackNode*> q;
    q.push(this);
    while (!q.empty()) {
      auto node = q.front(); q.pop();
      node->max_runtime = node->total_runtime;
      node->avg_runtime = node->total_runtime;
      MPI_Allreduce(MPI_IN_PLACE, &(node->total_runtime),
          1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
      MPI_Allreduce(MPI_IN_PLACE, &(node->max_runtime),
          1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
      MPI_Allreduce(MPI_IN_PLACE, &(node->avg_runtime),
          1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
      node->avg_runtime /= comm_size;
      /* There may be kernels that were called on rank 0 but were not
         called on certain other ranks.
         We will count these and add empty entries in the other ranks.
         There may also be kernels which were *not* called on rank 0 but
         *were* called on certain other ranks.
         We are *ignoring* these, because I can't think of an easy and
         scalable way to combine that data */
      int nchildren = int(node->children.size());
      MPI_Bcast(&nchildren, 1, MPI_INT, 0, MPI_COMM_WORLD);
      if (rank == 0) {
        for (auto& child : node->children) {
          int name_len = int(child.name.length());
          MPI_Bcast(&name_len, 1, MPI_INT, 0, MPI_COMM_WORLD);
          auto name = child.name;
          MPI_Bcast(&name[0], name_len, MPI_CHAR, 0, MPI_COMM_WORLD);
          int kind = child.kind;
          MPI_Bcast(&kind, 1, MPI_INT, 0, MPI_COMM_WORLD);
          q.push(const_cast<StackNode*>(&child));
        }
      } else {
        for (int i = 0; i < nchildren; ++i) {
          int name_len;
          MPI_Bcast(&name_len, 1, MPI_INT, 0, MPI_COMM_WORLD);
          std::string name(size_t(name_len), '?');
          MPI_Bcast(&name[0], name_len, MPI_CHAR, 0, MPI_COMM_WORLD);
          int kind;
          MPI_Bcast(&kind, 1, MPI_INT, 0, MPI_COMM_WORLD);
          auto child = node->get_child(std::move(name), StackKind(kind));
          q.push(child);
        }
      }
    }
#else
    std::queue<StackNode*> q;
    q.push(this);
    while (!q.empty()) {
      auto node = q.front(); q.pop();
      node->max_runtime = node->total_runtime;
      node->avg_runtime = node->total_runtime;
      for (auto& child : node->children) {
        q.push(const_cast<StackNode*>(&child));
      }
    }
#endif
  }
};

struct Allocation {
  std::string name;
  void* ptr;
  std::uint64_t size;
  StackNode* frame;
  Allocation(std::string&& name_in, void* ptr_in, std::uint64_t size_in,
      StackNode* frame_in):
    name(std::move(name_in)),ptr(ptr_in),size(size_in),frame(frame_in) {
  }
  bool operator<(Allocation const& other) const {
    if (size != other.size) return size > other.size;
    return ptr < other.ptr;
  }
};

struct Allocations {
  std::uint64_t total_size;
  std::set<Allocation> alloc_set;
  Allocations():total_size(0) {}
  void allocate(std::string&& name, void* ptr, std::uint64_t size,
      StackNode* frame) {
    auto res = alloc_set.emplace(
        Allocation(std::move(name), ptr, size, frame));
    assert(res.second);
    total_size += size;
  }
  void deallocate(std::string&& name, void* ptr, std::uint64_t size,
      StackNode* frame) {
    auto key = Allocation(std::move(name), ptr, size, frame);
    auto it = alloc_set.find(key);
    assert(it != alloc_set.end());
    total_size -= it->size;
    alloc_set.erase(it);
  }
  void print(std::ostream& os) {
#ifdef USE_MPI
    auto max_total_size = total_size;
    MPI_Allreduce(MPI_IN_PLACE, &max_total_size, 1,
        MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
    /* this bit of logic is here to break ties in case two
     * or more MPI ranks allocated the same (maximum) amount of
     * memory. the one with the lowest MPI rank will print
     * its snapshot */
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    auto min_max_rank = (max_total_size == total_size) ? rank : size;
    MPI_Allreduce(MPI_IN_PLACE, &min_max_rank, 1,
        MPI_INT, MPI_MIN, MPI_COMM_WORLD);
    assert(min_max_rank < size);
    if (rank != min_max_rank) return;
#endif
    os << "MAX BYTES ALLOCATED: " << total_size << '\n';
#ifdef USE_MPI
    os << "MPI RANK WITH MAX MEMORY: " << rank << '\n';
#endif
    os << "ALLOCATIONS AT TIME OF HIGH WATER MARK:\n";
    std::ios saved_state(nullptr);
    saved_state.copyfmt(os);
    os << std::fixed << std::setprecision(1);
    for (auto& allocation : alloc_set) {
      auto percent = double(allocation.size) / double(total_size) * 100.0;
      if (percent < 0.1) continue;
      std::string full_name = allocation.frame->get_full_name();
      if (full_name.empty()) full_name = allocation.name;
      else full_name = full_name + "/" + allocation.name;
      os << "  " << percent << "% " << full_name << '\n';
    }
    os << '\n';
    os.copyfmt(saved_state);
  }
};

struct State {
  StackNode stack_root;
  StackNode* stack_frame;
  Allocations current_allocations[NSPACES];
  Allocations hwm_allocations[NSPACES];
  State():stack_root(nullptr, "", STACK_REGION),stack_frame(&stack_root) {
    stack_frame->begin();
  }
  ~State() {
    auto end_time = now();
    if (stack_frame != &stack_root) {
      std::cerr << "Program ended before \"" << stack_frame->get_full_name()
                << "\" ended\n";
      abort();
    }
    stack_frame->end(end_time);
    auto inv_stack_root = stack_root.invert();
#ifdef USE_MPI
    stack_root.reduce_over_mpi();
    inv_stack_root.reduce_over_mpi();
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank == 0)
#endif
    {
      std::cout << "\nBEGIN KOKKOS PROFILING REPORT:\n";
      std::cout << "TOTAL TIME: " << stack_root.max_runtime << " seconds\n";
      std::cout << "TOP-DOWN TIME TREE:\n";
      std::cout << "<percent of total time> <percent MPI imbalance> <number of calls> <name> [type]\n";
      std::cout << "=================== \n";
      stack_root.print(std::cout);
      std::cout << "BOTTOM-UP TIME TREE:\n";
      std::cout << "<percent of total time> <percent MPI imbalance> <number of calls> <name> [type]\n";
      std::cout << "=================== \n";
      inv_stack_root.print(std::cout);
    }
    for (int space = 0; space < NSPACES; ++space) {
#ifdef USE_MPI
      if (rank == 0)
#endif
      {
        std::cout << "KOKKOS " << get_space_name(space) << " SPACE:\n";
        std::cout << "=================== \n";
      }
      hwm_allocations[space].print(std::cout);
    }
    print_process_hwm();
#ifdef USE_MPI
    if (rank == 0)
#endif
    {
      std::cout << "END KOKKOS PROFILING REPORT.\n";
    }
#ifdef USE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif
  }
  void begin_frame(const char* name, StackKind kind) {
    std::string name_str(name);
    stack_frame = stack_frame->get_child(std::move(name_str), kind);
    stack_frame->begin();
  }
  void end_frame(Now end_time) {
    stack_frame->end(end_time);
    stack_frame = stack_frame->parent;
  }
  std::uint64_t begin_kernel(const char* name, StackKind kind) {
    begin_frame(name, kind);
    return reinterpret_cast<std::uint64_t>(stack_frame);
  }
  void end_kernel(std::uint64_t kernid) {
    auto end_time = now();
    auto expect_node = reinterpret_cast<StackNode*>(kernid);
    if (expect_node != stack_frame) {
      std::cerr << "Expected \"" << stack_frame->get_full_name()
                << "\" to end, got different kernel ID\n";
      abort();
    }
    end_frame(end_time);
  }
  void push_region(const char* name) {
    begin_frame(name, STACK_REGION);
  }
  void pop_region() {
    end_frame(now());
  }
  void allocate(Space space, const char* name, void* ptr, std::uint64_t size) {
    current_allocations[space].allocate(
        std::string(name), ptr, size, stack_frame);
    if (current_allocations[space].total_size > hwm_allocations[space].total_size) {
      hwm_allocations[space] = current_allocations[space];
    }
  }
  void deallocate(Space space, const char* name, void* ptr, std::uint64_t size) {
    current_allocations[space].deallocate(
        std::string(name), ptr, size, stack_frame);
  }
  void begin_deep_copy(
      Space, const char* dst_name, const void*,
      Space, const char* src_name, const void*,
      std::uint64_t) {
    std::string frame_name;
    frame_name += "\"";
    frame_name += dst_name;
    frame_name += "\"=\"";
    frame_name += src_name;
    frame_name += "\"";
    begin_frame(frame_name.c_str(), STACK_COPY);
  }
  void end_deep_copy() {
    end_frame(now());
  }
};

State* global_state = nullptr;

}  // end anonymous namespace

extern "C" void kokkosp_init_library(
    int loadseq, uint64_t version, uint32_t ndevinfos, KokkosPDeviceInfo* devinfos) {
  (void)loadseq;
  (void)ndevinfos;
  (void)devinfos;
  if (version != 20150628) {
    std::cerr << "kokkosp_init_library: version " << version << " != 20150628\n";
    abort();
  }
  global_state = new State();
}

extern "C" void kokkosp_finalize_library() {
  delete global_state;
  global_state = nullptr;
}

extern "C" void kokkosp_begin_parallel_for(
    const char* name, std::uint32_t devid, std::uint64_t* kernid) {
  (void) devid;
  *kernid = global_state->begin_kernel(name, STACK_FOR);
}

extern "C" void kokkosp_begin_parallel_reduce(
    const char* name, std::uint32_t devid, std::uint64_t* kernid) {
  (void) devid;
  *kernid = global_state->begin_kernel(name, STACK_REDUCE);
}

extern "C" void kokkosp_begin_parallel_scan(
    const char* name, std::uint32_t devid, std::uint64_t* kernid) {
  (void) devid;
  *kernid = global_state->begin_kernel(name, STACK_SCAN);
}

extern "C" void kokkosp_end_parallel_for(std::uint64_t kernid) {
  global_state->end_kernel(kernid);
}

extern "C" void kokkosp_end_parallel_reduce(std::uint64_t kernid) {
  global_state->end_kernel(kernid);
}

extern "C" void kokkosp_end_parallel_scan(std::uint64_t kernid) {
  global_state->end_kernel(kernid);
}

extern "C" void kokkosp_push_profile_region(const char* name) {
  global_state->push_region(name);
}

extern "C" void kokkosp_pop_profile_region() {
  global_state->pop_region();
}

extern "C" void kokkosp_allocate_data(
    SpaceHandle handle, const char* name, void* ptr, uint64_t size) {
  auto space = get_space(handle);
  global_state->allocate(space, name, ptr, size);
}

extern "C" void kokkosp_deallocate_data(
    SpaceHandle handle, const char* name, void* ptr, uint64_t size) {
  auto space = get_space(handle);
  global_state->deallocate(space, name, ptr, size);
}

extern "C" void kokkosp_begin_deep_copy(
    SpaceHandle dst_handle, const char* dst_name, const void* dst_ptr,
    SpaceHandle src_handle, const char* src_name, const void* src_ptr,
    uint64_t size) {
  auto dst_space = get_space(dst_handle);
  auto src_space = get_space(src_handle);
  global_state->begin_deep_copy(dst_space, dst_name, dst_ptr,
      src_space, src_name, src_ptr, size);
}

extern "C" void kokkosp_end_deep_copy() {
  global_state->end_deep_copy();
}

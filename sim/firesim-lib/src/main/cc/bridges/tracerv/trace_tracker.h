#include "tracerv_processing.h"
#include <queue>

//#define INDENT_SPACES
struct token_t
{
  // from the bridge
  uint64_t cycle_count;
  uint64_t iaddr;
  uint64_t inst;
  uint64_t satp;
  uint8_t priv;

  // user, kernel, hypervisor instruction
  uint8_t mode; 
  // post-matching instr_meta indicates the info about instruction
  Instr* instr_meta;
};


struct bin_page_pair_t
{
  ObjdumpedBinary *bin;
  uint64_t page_base;
};

constexpr int USER_KEY = 0;
constexpr int KERNEL_KEY = 1;
constexpr int HYPER_KEY = 2;
constexpr int MACHINE_KEY = 3;



class LabelMeta {
public:
  std::string label;
  uint64_t start_cycle;
  uint64_t end_cycle;
  uint64_t indent;
  bool asm_sequence;

  LabelMeta() { this->asm_sequence = false; }

  void pre_print(FILE *tracefile) {
#ifdef INDENT_SPACES
    std::string ind(indent, ' ');
    fprintf(tracefile,
            "%sStart label: %s at %" PRIu64 " cycles.\n",
            ind.c_str(),
            label.c_str(),
            start_cycle);
#else
    fprintf(tracefile,
            "Indent: %" PRIu64 ", Start label: %s, At cycle: %" PRIu64 "\n",
            indent,
            label.c_str(),
            start_cycle);
#endif
  }

  void post_print(FILE *tracefile) {
#ifdef INDENT_SPACES
    std::string ind(indent, ' ');
    fprintf(tracefile,
            "%sEnd label: %s at %" PRIu64 " cycles.\n",
            ind.c_str(),
            label.c_str(),
            end_cycle);
#else
    fprintf(tracefile,
            "Indent: %" PRIu64 ", End label: %s, End cycle: %" PRIu64 "\n",
            indent,
            label.c_str(),
            end_cycle);
#endif
  }
};

class TraceTracker {
private:
  const uint8_t BUFFER_SIZE = 200;
  const uint8_t MATCHING_DEPTH = 6;
  std::map <uint8_t, std::vector<ObjdumpedBinary *>> bins_dump_map; // maps from <user: 0, kernel: 1, hyper: 2>  to Vectors of ObjdumpedBinaries
  std::vector<LabelMeta *> label_stack;
  std::deque<struct token_t> retired_buffer;
  FILE *tracefile;
  Instr *last_instr;
  std::map<uint64_t, std::map<uint64_t, struct bin_page_pair_t>> satp_page_cache;
  std::vector<std::map<uint64_t, std::vector<struct bin_page_pair_t>>> offset_inst_to_page;
  bool TraceTracker::filterBuffer(struct token_t *, uint64_t);
  bool TraceTracker::mapContains(std::vector<struct bin_page_pair_t>, uint64_t, ObjdumpedBinary *);
public:
  TraceTracker(std::string binary_with_dwarf, FILE *tracefile);
  void addInstruction(token_t token, bool buffer_flush_mode);
  bool matchInstruction(struct token_t &token);
};

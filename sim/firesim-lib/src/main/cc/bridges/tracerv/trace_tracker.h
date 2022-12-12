#ifndef __TRACE_TRACKER_H
#define __TRACE_TRACKER_H
#include "tracerv_processing.h"
//#define INDENT_SPACES


struct token_t
{
  // from the bridge
  uint64_t cycle_count;
  uint64_t iaddr;
  uint64_t inst;
  uint64_t satp;
  uint8_t priv;
  // Assign the 
  ObjdumpedBinary* bin;
  // post-matching instr_meta indicates the info about instruction
  Instr* instr_meta;
  // page base
  uint64_t page_base;
};

struct bin_page_pair_t
{
  ObjdumpedBinary *bin;
  uint64_t page_base;
};


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
  std::vector<LabelMeta *> label_stack;
  FILE *tracefile;
  Instr *last_instr;
public:
  TraceTracker(FILE *tracefile);
  void addInstruction(struct token_t token);
};

#endif // ifndef __TRACE_TRACKER_H
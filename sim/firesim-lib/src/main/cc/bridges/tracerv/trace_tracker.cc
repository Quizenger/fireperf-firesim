#include "trace_tracker.h"
#include <filesystem>
#include "tracerv_processing.h"
#include <regex>


#define PAGE_OFFSET_BITS 4096

//#define TRACETRACKER_LOG_PC_REGION
TraceTracker::TraceTracker(std::string path_to_dir_with_dwarfs, FILE *tracefile) {
  // Map objdumpedbinary object of kernel dwarf
  this->bins_dump_map[KERNEL_KEY].push_back(new ObjdumpedBinary(path_to_dir_with_dwarfs + std::string("/kernel/fireperf-userspace-bin-dwarf")));

  // Map objdumpedbinary object of user dwarfs
  size_t BUFF_SIZE = 100;
  char *buf = (char*) malloc(sizeof(char) * BUFF_SIZE);
  FILE *f;
  for (int i = 0; i < 1024; i++) {
    std::map<uint64_t, std::vector<struct bin_page_pair_t>> m;
    this->offset_inst_to_page.at(i) = m;
  }

  for (auto &user_file : std::filesystem::directory_iterator(path_to_dir_with_dwarfs + std::string("/user"))) {
    // create object dumped binary object per binary dwarf file
    ObjdumpedBinary *dumped = new ObjdumpedBinary(user_file.string() + std::string("/dwarf"));
    this->bins_dump_map[USER_KEY].push_back(dumped);
    // read the corresponding hexdumped file to map instructions to binary and base addresses pairs
    f = fopen(user_file.string() + std::string("/hex"), "r");
    if (!f) {
      perror("hex file open failed.");
      return;
    }
    // parse through the hexdumped file for each instructions and addresses and push them into the map
    while (getline(&buf, &BUFF_SIZE, f) != -1) {
      char *ptr = strtok(buf, " ");
      uint64_t addr = strtoll(ptr, NULL, 16);
      uint64_t instr;
      for (int i = 0; i < 4; i++) {
        ptr = strtok(NULL, " ");
        instr = strtoll(ptr, NULL, 16);
        // 4096 for 4k pages, right shift by 2 to get rid of byte offset, and i for each instruction 
        int offset_index = ((addr % PAGE_OFFSET_BITS) >> 2) + i;
        struct bin_page_pair_t pair;
        pair.bin = dumped;
        // upper bits are the page address bits 
        pair.page_base = (addr >> 12) << 12;
        this->offset_inst_to_page.at(offset_index)[instr].push_back(pair);
      }
    }
  }
  // TODO: support hypervisor
  this->tracefile = tracefile;
}

/*
 Return the Instr object of each token.
 Kernel: Calculate offset and lookup in file. 
 User:
 First check buffer entry; 
 If hit, simply find Instr, 
 otherwise exhaustive search of all "user" instrs with that asid+ppn
   + mark all in buffer
   + add cache entry to user_ppn_bin_cache 
*/
bool TraceTracker::matchInstruction(struct token_t &token) {
  ObjdumpedBinary* kernel_objdump = this->bins_dump_map[KERNEL_KEY].at(0);
  if (token.iaddr >= kernel_objdump->baseaddr && (token.iaddr - kernel_objdump->baseaddr) <= kernel_objdump->progtext.size()) {
    /* Kernel */
    return kernel_objdump->progtext[token.iaddr - kernel_objdump->baseaddr];
  } else {
    /* user match by looking at buffer AND updating buffer */
    if (token.instr_meta != nullptr) {
      return true;
    }
    std::vector<struct bin_page_pair_t> possible_sites = this->offset_inst_to_page.at((token.iaddr%PAGE_OFFSET_BITS) >> 2)[token.inst];
    /* lookup in the cache for possible fast resolution and verify it was a hit by comparing the structs */
    if (this->satp_page_cache.count(token.satp) != 0 && this->satp_page_cache[token.satp].count(token.iaddr >> 12) != 0) {
      struct bin_page_pair_t hit = this->satp_page_cache[token.satp][token.iaddr >> 12];
      // possible bin is a hit but needs to be verified using the instruction bits
      for (auto const &item : possible_sites) {
        if (item.bin == hit.bin && item.page_base == hit.page_base) {
          token.instr_meta = hit.bin->progtext[token.iaddr%PAGE_OFFSET_BITS + hit.page_base - hit.bin->baseaddr];
          return true;
        }
      }
    } 
    /* regular matching if not found in cache or found some conflicting entries  and */
    if (possible_sites.size() == 0) {
      /* failed to find any matching binary, USERSPACE ALL */
      return false;

    } else if (possible_sites.size() == 1) {
      /* best case, found the unique match */
      token.instr_meta = possible_sites.at(0).bin->progtext[token.iaddr%PAGE_OFFSET_BITS + possible_sites.at(0).page_base - possible_sites.at(0).bin->baseaddr];
      return true;
    } else {
      /* no unique possible_sites were found */
      std::vector<struct token_t *> matching_vec;
      for (auto it = this->retired_buffer.begin(); matching_vec.size() < MATCHING_DEPTH && it != this->retired_buffer.end(); it++) { 
        if (filterBuffer(&(*it), token.satp)) {
          matching_vec.push_back(&(*it));
        }
      }
      std::vector<struct bin_page_pair_t> matched_sites;
      for (auto const &s : possible_sites) {
        for (size_t i = 0; i < matching_vec.size(); i++) {
          struct token_t *m = matching_vec.at(i);
          uint64_t p = ((m->iaddr >> 12) << 12) - ((token.iaddr >> 12) << 12) + s.page_base;
          std::vector<struct bin_page_pair_t> v = this->offset_inst_to_page[m->iaddr%PAGE_OFFSET_BITS][m->inst];
          if(!TraceTracker::mapContains(v, p, s.bin)) {
            break;
          }
          if (i == matching_vec.size() - 1) {
            matched_sites.push_back(s);
          }
        }
      }
      // check the number of matches sites; check if matched_sites binaries are the same in case there are more than 1
      if (matched_sites.size() == 0)  {
        // failed to find any matching
        token.instr_meta = new Instr();
        token.instr_meta->function_name = std::string("USERSPACE_ALL");
        return false;
      } else if (matched_sites.size() == 1) {
        token.instr_meta = matched_sites.at(0).bin->progtext[token.iaddr%PAGE_OFFSET_BITS + matched_sites.at(0).page_base - matched_sites.at(0).bin->baseaddr];
        return true;
      } else {
        for (size_t i = 1; i < matched_sites.size(); i++) {
          if (matched_sites.at(0).bin != matched_sites.at(i).bin) {
            token.instr_meta = new Instr();
            token.instr_meta->function_name = std::string("USERSPACE_ALL");
            return false;
          } 
        }
        token.instr_meta = new Instr();
        token.instr_meta->function_name.assign(matched_sites.at(0).bin->bin_name);
        return true;
      }
    }
  }
}

bool TraceTracker::mapContains(std::vector <struct bin_page_pair_t> v, uint64_t page_base, ObjdumpedBinary *bin) {
  for (auto const &el : v) {
    if (el.bin == bin && el.page_base == page_base) {
      return true;
    }
  }
  return false;
}

bool TraceTracker::filterBuffer(struct token_t* token, uint64_t satp) {
  ObjdumpedBinary* kernel_objdump = this->bins_dump_map[KERNEL_KEY].at(0);
  if ((token->iaddr >= kernel_objdump->baseaddr 
    && (token->iaddr - kernel_objdump->baseaddr) <= kernel_objdump->progtext.size())
    || satp != token->satp) 
   return false;
  return true; 
}

void TraceTracker::addInstruction(struct token_t token, bool buffer_flush_mode) {
  // populate tokens into the retired buffer
  uint64_t cycle = token.cycle_count;
  uint64_t inst_addr = token.iaddr;
  this->retired_buffer.push_back(token);
  if (!(this->retired_buffer.size() == BUFFER_SIZE) or buffer_flush_mode) {
    return;
  }
  struct token_t cur_token = this->retired_buffer.front();
  this->retired_buffer.pop_front();

#ifdef TRACETRACKER_LOG_PC_REGION /* What does this do??? */ // SKIPPED FOR NOW, BUT CHANGE IF NEEDED
  if (!this_instr) {
    /* 
      CURR BEHAVIOR:
        When we don't find instr - just mark as userspace.
      DESIRED BEHAVIOR:
        When we don't find instr - we can at least map which mode it came from.
    */
    fprintf(
        this->tracefile, "addr:%" PRIx64 ", fn:%s\n", inst_addr, "USERSPACE");
  } else {
    fprintf(this->tracefile,
            "addr:%" PRIx64 ", fn:%s\n",
            inst_addr,
            this_instr->function_name.c_str());
  }
  return;
#endif

  std::string label;
  if (TraceTracker::matchInstruction(cur_token)) {
    label = cur_token.instr_meta->function_name;
  } else {
    label = std::string("USERSPACE_ALL");
  }
  Instr *this_instr = cur_token.instr_meta;
    /* 
      CURR BEHAVIOR:
        If prev instr was userspace all, end it
        Eventually either this instr gets mapped correctly or 
        it is also added as userspace all, and then gets popped and concatenated with the rest of 
        userspace all.
      DESIRED BEHAVIOR;
       TBD
    */
    if ((label_stack.size() > 0) &&
        (std::string("USERSPACE_ALL")
             .compare(label_stack[label_stack.size() - 1]->label) == 0)) {
      LabelMeta *pop_label = label_stack[label_stack.size() - 1];
      label_stack.pop_back();
      pop_label->post_print(this->tracefile);
      delete pop_label;
    }

    if ((label_stack.size() > 0) &&
        (label.compare(label_stack[label_stack.size() - 1]->label) == 0)) {
      /* 
        CURR BEHAVIOR:
          If in same label/fn => update end cycle - don't pop?
        DESIRED BEHAVIOR;
          SAME
      */ 
      LabelMeta *last_label = label_stack[label_stack.size() - 1];
      last_label->end_cycle = cycle;
    } else {
      if ((label_stack.size() > 0) and this_instr->in_asm_sequence and
          label_stack[label_stack.size() - 1]->asm_sequence) {
        /* 
          CURR BEHAVIOR:
            If curr and prev are both asm_sequences, end prev, add curr at same indent and depend
            on coalescing if reqd.
          DESIRED BEHAVIOR;
            SAME
        */
        LabelMeta *pop_label = label_stack[label_stack.size() - 1];
        label_stack.pop_back();
        pop_label->post_print(this->tracefile);
        delete pop_label;

        LabelMeta *new_label = new LabelMeta();
        new_label->label = label;
        new_label->start_cycle = cycle;
        new_label->end_cycle = cycle;
        new_label->indent = label_stack.size() + 1;   /* Same Indent (-1 +1) */
        new_label->asm_sequence = this_instr->in_asm_sequence;
        label_stack.push_back(new_label);
        new_label->pre_print(this->tracefile);
      } else if ((label_stack.size() > 0) and
                 (this_instr->is_callsite or !(this_instr->is_fn_entry))) {
        /*
          CURR BEHAVIOR:
            If not an entry point into fn or if callsite => middle of fn or return to fn
              a. middle: label on top of stack will be the same as curr label
              b. return: now returning from fn call, 
                          need to unwind stack and get back to where this fn issued call

              Stuff like sequential execution, fn call and return, tail recursion and goto 
              all accounted for here
            
          DESIRED BEHAVIOR:
            SAME
        */
        uint64_t unwind_start_level = (uint64_t)(-1);
        while (
            (label_stack.size() > 0) and
            (label_stack[label_stack.size() - 1]->label.compare(label) != 0)) {
          LabelMeta *pop_label = label_stack[label_stack.size() - 1];
          label_stack.pop_back();
          pop_label->post_print(this->tracefile);
          if (unwind_start_level == (uint64_t)(-1)) {
            unwind_start_level = pop_label->indent;
          }
          delete pop_label;
          /* found curr label bafter stack unwinding => update end cycle */
          if (label_stack.size() > 0) {
            LabelMeta *last_label = label_stack[label_stack.size() - 1];
            last_label->end_cycle = cycle;
          }
        }
        if (label_stack.size() == 0) {
          fprintf(this->tracefile,
                  "WARN: STACK ZEROED WHEN WE WERE LOOKING FOR LABEL: %s, "
                  "iaddr 0x%" PRIx64 "\n",
                  label.c_str(),
                  inst_addr);
          fprintf(this->tracefile,
                  "WARN: is_callsite was: %d, is_fn_entry was: %d\n",
                  this_instr->is_callsite,
                  this_instr->is_fn_entry);
          fprintf(this->tracefile,
                  "WARN: Unwind started at level: dec %" PRIu64 "\n",
                  unwind_start_level);
          fprintf(this->tracefile, "WARN: Last instr was\n");
          this->last_instr->printMeFile(this->tracefile, std::string("WARN: "));
        }
      } else {
        /* Add new label */
        LabelMeta *new_label = new LabelMeta();
        new_label->label = label;
        new_label->start_cycle = cycle;
        new_label->end_cycle = cycle;
        new_label->indent = label_stack.size() + 1;
        new_label->asm_sequence = this_instr->in_asm_sequence;
        label_stack.push_back(new_label);
        new_label->pre_print(this->tracefile);
      }
    }
    this->last_instr = this_instr;
}

#ifdef TRACERV_TOP_MAIN
int main() {
  std::string tracefile = "/home/centos/trace2/TRACEFILE";

  std::ifstream is(tracefile);
  std::string line;

  TraceTracker *t =
      new TraceTracker("/home/centos/trace2/vmlinux-dwarf", stdout);
  while (getline(is, line)) {
    std::string addr_str = line.substr(0, 16);
    std::string cycle_str = line.substr(16, 32);
    uint64_t addr = (uint64_t)strtoull(addr_str.c_str(), NULL, 16);
    uint64_t cycle = (uint64_t)strtoull(cycle_str.c_str(), NULL, 16);
    t->addInstruction(addr, cycle);
  }
}
#endif

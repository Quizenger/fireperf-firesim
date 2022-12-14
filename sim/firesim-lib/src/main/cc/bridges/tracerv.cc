// See LICENSE for license details
#ifdef TRACERVBRIDGEMODULE_struct_guard

#include "tracerv.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/mman.h>

// put FIREPERF in a mode that writes a simple log for processing later.
// useful for iterating on software side only without re-running on FPGA.
// #define FIREPERF_LOGGER

constexpr uint64_t valid_mask = (1ULL << 40);
constexpr uint64_t BYTES_PER_PAGE = 4096;
constexpr uint64_t INSTRUCTION_PER_PAGE = 2048;
constexpr uint64_t DRAM_ROOT = 0x80000000;
size_t BUFF_SIZE = 300;
FILE* fireperf_logger;

/*
  tracerv_t file structure assumption:
  --/{dwarf_dir_name}
  ---/kernel
  ------/dwarf (file)
  ---/user
  ------/{program_1_name}
  --------/dwarf (file)
  --------/hex   (file)
  ------/{program_2_name}
  --------/dwarf (file)
  --------/hex   (file)
*/

tracerv_t::tracerv_t(simif_t *sim,
          std::vector<std::string> &args,
          TRACERVBRIDGEMODULE_struct *mmio_addrs,
          const int stream_idx,
          const int stream_depth,
          const unsigned int max_core_ipc,
          const char *const clock_domain_name,
          const unsigned int clock_multiplier,
          const unsigned int clock_divisor,
          int tracerno, 
          int matching_depth /*= 3*/, uint8_t userspace /*= 1*/)
  : bridge_driver_t(sim), mmio_addrs(mmio_addrs), stream_idx(stream_idx),
stream_depth(stream_depth), max_core_ipc(max_core_ipc),
clock_info(clock_domain_name, clock_multiplier, clock_divisor) {
  // Biancolin: move into elaboration
  assert(this->max_core_ipc <= 7 &&
                "TracerV only supports cores with a maximum IPC <= 7");
  const char *tracefilename = "TRACEFILE";
  const char *dwarf_dir_name = "top";
  const char *dwarf_file_name = NULL;

  fireperf_logger = fopen("fireperf_logger", "w");

  // this->buffer_flush_mode = true; // to flush the retired buffer
  this->trace_trigger_start = 0;
  this->trace_trigger_end = ULONG_MAX;
  this->trigger_selector = 0;

  this->tracefilename = "TRACEFILE";
  this->dwarf_dir_name = "top";
  this->dwarf_file_name = "";

  this->MATCHING_DEPTH = matching_depth;
  this->userspace = userspace;

  long outputfmtselect = 2;

  std::string suffix = std::string("=");
  std::string tracefile_arg = std::string("+tracefile") + suffix;
  std::string tracestart_arg = std::string("+trace-start") + suffix;
  std::string traceend_arg = std::string("+trace-end") + suffix;
  std::string traceselect_arg = std::string("+trace-select") + suffix;
  // Testing: provides a reference file to diff the collected trace against
  std::string testoutput_arg = std::string("+trace-test-output");
  // Formats the output before dumping the trace to file
  std::string humanreadable_arg = std::string("+trace-humanreadable");

  std::string trace_output_format_arg =
      std::string("+trace-output-format") + suffix;
  std::string dwarf_file_arg = std::string("+dwarf-file-name") + suffix;

  for (auto &arg : args) {
    /* 
     *  Commenting our for fireperf userspace integration
     *  if (arg.find(tracefile_arg) == 0) {
     *    tracefilename = const_cast<char *>(arg.c_str()) + tracefile_arg.length();
     *    this->tracefilename = std::string(tracefilename);
     *  }
     */
    if (arg.find(traceselect_arg) == 0) {
      char *str = const_cast<char *>(arg.c_str()) + traceselect_arg.length();
      this->trigger_selector = atol(str);
    }
    // These next two arguments are overloaded to provide trigger start and
    // stop condition information based on setting of the +trace-select
    if (arg.find(tracestart_arg) == 0) {
      // Start and end cycles are given in decimal
      char *str = const_cast<char *>(arg.c_str()) + tracestart_arg.length();
      this->trace_trigger_start = this->clock_info.to_local_cycles(atol(str));  
      // PCs values, and instruction and mask encodings are given in hex
      uint64_t mask_and_insn = strtoul(str, NULL, 16);
      this->trigger_start_insn = (uint32_t)mask_and_insn;
      this->trigger_start_insn_mask = mask_and_insn >> 32;
      this->trigger_start_pc = mask_and_insn;
    }
    if (arg.find(traceend_arg) == 0) {
      char *str = const_cast<char *>(arg.c_str()) + traceend_arg.length();
      this->trace_trigger_end = this->clock_info.to_local_cycles(atol(str));

      uint64_t mask_and_insn = strtoul(str, NULL, 16);
      this->trigger_stop_insn = (uint32_t)mask_and_insn;
      this->trigger_stop_insn_mask = mask_and_insn >> 32;
      this->trigger_stop_pc = mask_and_insn;
    }
    if (arg.find(testoutput_arg) == 0) {
      this->test_output = true;
    }
    if (arg.find(trace_output_format_arg) == 0) {
      char *str =
        const_cast<char *>(arg.c_str()) + trace_output_format_arg.length();
      outputfmtselect = atol(str);
    }
    if (arg.find(dwarf_file_arg) == 0) {
      dwarf_file_name =
        const_cast<char *>(arg.c_str()) + dwarf_file_arg.length();
      this->dwarf_file_name = std::string(dwarf_file_name);
    }
  } 

  if (tracefilename) {
    // final tracefile is created here and populated in the destructor by concatenating 
    //user space tracefiles and kernel tracefile
    std::string tfname = std::string(tracefilename) + std::string("-C") + std::to_string(tracerno);
    FILE *f = fopen(tfname.c_str(), "w");
    if (!f) {
      fprintf(stderr, "Could not open Trace log file: %s\n", tfname.c_str());
      abort();
    }
    this->tracefiles["final"] = f;
    // giving no tracefilename means we will create NO tracefiles
    tfname = std::string(tracefilename) + std::string("-kernel") + std::string("-C") +
                         std::to_string(tracerno);
    f = fopen(tfname.c_str(), "w");
    if (!f) {
      fprintf(stderr, "Could not open Trace log file: %s\n", tfname.c_str());
      abort();
    }
    this->tracefiles["kernel"] = f;
    tfname = std::string(tracefilename) + std::string("-misc") + std::string("-C") + std::to_string(tracerno);
    f = fopen(tfname.c_str(), "w");
    if (!f) {
      fprintf(stderr, "Could not open Trace log file: %s\n", tfname.c_str());
      abort();
    }
    this->tracefiles["misc"] = f;
    std::string s = this->dwarf_dir_name + std::string("/user");
    auto l = std::filesystem::directory_iterator(this->dwarf_dir_name + std::string("/user"));
    for (auto const &user_program : std::filesystem::directory_iterator(this->dwarf_dir_name + std::string("/user"))) {
      std::string user_program_name = std::filesystem::path(user_program).filename();
      tfname = std::string(tracefilename) + std::string("-") + user_program_name + 
                std::string("-C") + std::to_string(tracerno);
      f = fopen(tfname.c_str(), "w");
      if (!f) {
        fprintf(stderr, "Could not open Trace log file: %s\n", tfname.c_str());
        abort();
      }
      this->tracefiles[user_program_name] = f;
    }
    //fputs(this->clock_info.file_header().c_str(), this->tracefiles["final"]);

    // This must be kept consistent with config_runtime.ini's output_format.
    // That file's comments are the single source of truth for this.
    if (outputfmtselect == 0) {
      this->human_readable = true;
      this->fireperf = false;
    } else if (outputfmtselect == 1) {
      this->human_readable = false;
      this->fireperf = false;
    } else if (outputfmtselect == 2) {
      this->human_readable = false;
      this->fireperf = true;
    } else {
      fprintf(stderr, "Invalid trace format arg\n");
    }
  } else {
    fprintf(
        stderr,
        "TraceRV %d: Tracing disabled, since +tracefile was not provided.\n",
        tracerno);
    this->trace_enabled = false;
  }

  if (this->fireperf) {
    if (this->dwarf_file_name.compare("") == 0) {
      fprintf(stderr, "+fireperf specified but no +dwarf-file-name given\n");
      abort();
    }
    if (this->dwarf_dir_name.compare("") == 0) {
      fprintf(stderr, "+fireperf specified but no +dwarf-dir-name given\n");
      abort();
    }
    this->trace_trackers["kernel"] = new TraceTracker(this->tracefiles["kernel"]);
    this->trace_trackers["misc"] = new TraceTracker(this->tracefiles["misc"]);
    
    for (auto const &user_program : std::filesystem::directory_iterator(this->dwarf_dir_name + std::string("/user"))) {
      std::string user_program_name = std::filesystem::path(user_program).filename();
      this->trace_trackers[user_program_name] = 
        new TraceTracker(this->tracefiles[user_program_name]);
    } 
  }

  // Initilize the vector of maps of offset_inst_to_page 
  for (int i = 0; i < INSTRUCTION_PER_PAGE; i++) {
    std::map<uint64_t, std::vector<struct bin_page_pair_t>> m; 
    // std::map<uint64_t, std::vector<struct bin_page_pair_t>>* m = new std::map<uint64_t, std::vector<struct bin_page_pair_t>>;
    this->offset_inst_to_page.push_back(m);
  }

  // Map objdumpedbinary object of kernel dwarf
  this->kernel_objdump = new ObjdumpedBinary(this->dwarf_dir_name + std::string("/kernel/dwarf"));
  
  char *buf = (char*) malloc(sizeof(char) * BUFF_SIZE);
  char *buf_free = buf;
  char *buf_cpy = buf;
  if (NULL == buf) {
    perror("read buf malloc failed.");
    return;
  }
  FILE *f;

  std::cerr << "just testing " << std::endl;


  for (auto &user_program : std::filesystem::directory_iterator(this->dwarf_dir_name + std::string("/user"))) {
    std::string user_program_path = std::filesystem::path(user_program).string();
    // create object dumped binary object per binary dwarf file
    ObjdumpedBinary *dumped = new ObjdumpedBinary(user_program_path + std::string("/dwarf"));
    fprintf(stderr, "\n%s\n", dumped->bin_name.c_str());
    // read the corresponding hexdumped file to map instructions to binary and base addresses pairs
    f = fopen((user_program_path + std::string("/hex")).c_str(), "r");
    if (!f) {
      perror("hex file open failed.");
      return;
    }
    // parse through the hexdumped file for each instructions and addresses and push them into the map
    while (getline(&buf_cpy, (size_t *) &BUFF_SIZE, f) != -1) {
      buf = buf_cpy;
      char *ptr = strtok(buf, " ");
      if (NULL == ptr)
        continue;
      uint64_t addr = strtoull(ptr, NULL, 16);
      
      if (addr == 0x1048e) 
        fprintf(stderr, "seen addr 1048e in hex");
      
      uint64_t instr;
      ptr = strtok(NULL, " ");
      if (NULL == ptr)
        continue;
      instr = strtoull(ptr, NULL, 16);
      
      if (addr == 0x1048e) 
        fprintf(stderr, "seen addr 1048e in hex with instr: %x", instr);
      
      // 4096 for 4k pages, right shift by 2 to get rid of byte offset
      uint64_t offset_index = ((addr % BYTES_PER_PAGE) >> 1);
      struct bin_page_pair_t pair;
      pair.bin = dumped;
      // upper bits are the page address bits 
      pair.page_base = (addr >> 12) << 12;
      
      try {
        this->offset_inst_to_page.at(offset_index)[instr].push_back(pair);
        //auto inst_to_page = this->offset_inst_to_page.at(offset_index);
        //auto bin_page_pair_vec = inst_to_page[instr];
        //bin_page_pair_vec.push_back(pair);
      }
      catch (const std::exception& e) {
        std::cerr << "Exception in hex stuff: " << e.what() << std::endl;
      }
      /*
      catch (const std::out_of_range& oor) {
        std::cerr << "Out of Range error (inst_to_page): " << oor.what() << '\n';
      }
      catch (std::bad_alloc& ba)
      {
        std::cerr << "bad_alloc caught (bin_page_vec): " << ba.what() << '\n';
      }
      */

      //this->offset_inst_to_page.at(offset_index)[instr].push_back(pair);
    }
  }
  std::vector<struct bin_page_pair_t> possible_sites = this->offset_inst_to_page.at((0x1048e % BYTES_PER_PAGE) >> 1)[0x0f67ff0ef];
  std::cerr << "bin page mapping comin' up" << std::endl;
  std::cerr << "size = " << possible_sites.size() << std::endl;
  
  for (auto const &s : possible_sites) {
    std::cerr << "bin: " << s.bin->bin_name << " page: " << s.page_base << std::endl;
  }
  free(buf_free);
}

void tracerv_t::copyFile(FILE* to, FILE* from) {
  int r = fseek(from, 0, SEEK_SET);
  if (r == -1) {
    perror("Fseek error.");
    exit(1);
  }
  char c;
  c = fgetc(from);
  size_t read = 0;
  while (c != EOF) {
    read += fputc(c, to);
    c = fgetc(from);
  }
}

tracerv_t::~tracerv_t() {
  FILE *final = this->tracefiles["final"];
  if (this->tracefiles.size() > 0) {
    for (auto const &f : this->tracefiles) {
      if (f.first != "final") {
        tracerv_t::copyFile(final, f.second);
        // maybe use the below one once you are sure it works
      }
    }
    for (auto const &f : this->tracefiles) {
      fclose(f.second);
    }
  }
  fclose(final);
  // free(this->mmio_addrs);
}

void tracerv_t::init() {
  if (!this->trace_enabled) {
    // Explicitly disable token collection in the bridge if no tracefile was
    // provided to improve FMR
    write(this->mmio_addrs->traceEnable, 0);
  }

  // Configure the trigger even if tracing is disabled, as other
  // instrumentation, like autocounter, may use tracerv-hosted trigger sources.
  if (this->trigger_selector == 1) {
    write(this->mmio_addrs->triggerSelector, this->trigger_selector);
    write(this->mmio_addrs->hostTriggerCycleCountStartHigh,
          this->trace_trigger_start >> 32);
    write(this->mmio_addrs->hostTriggerCycleCountStartLow,
          this->trace_trigger_start & ((1ULL << 32) - 1));
    write(this->mmio_addrs->hostTriggerCycleCountEndHigh,
          this->trace_trigger_end >> 32);
    write(this->mmio_addrs->hostTriggerCycleCountEndLow,
          this->trace_trigger_end & ((1ULL << 32) - 1));
    printf("TracerV: Trigger enabled from %lu to %lu cycles\n",
           trace_trigger_start,
           trace_trigger_end);
  } else if (this->trigger_selector == 2) {
    write(this->mmio_addrs->triggerSelector, this->trigger_selector);
    write(this->mmio_addrs->hostTriggerPCStartHigh,
          this->trigger_start_pc >> 32);
    write(this->mmio_addrs->hostTriggerPCStartLow,
          this->trigger_start_pc & ((1ULL << 32) - 1));
    write(this->mmio_addrs->hostTriggerPCEndHigh, this->trigger_stop_pc >> 32);
    write(this->mmio_addrs->hostTriggerPCEndLow,
          this->trigger_stop_pc & ((1ULL << 32) - 1));
    printf("TracerV: Trigger enabled from instruction address %lx to %lx\n",
           trigger_start_pc,
           trigger_stop_pc);
  } else if (this->trigger_selector == 3) {
    write(this->mmio_addrs->triggerSelector, this->trigger_selector);
    write(this->mmio_addrs->hostTriggerStartInst, this->trigger_start_insn);
    write(this->mmio_addrs->hostTriggerStartInstMask,
          this->trigger_start_insn_mask);
    write(this->mmio_addrs->hostTriggerEndInst, this->trigger_stop_insn);
    write(this->mmio_addrs->hostTriggerEndInstMask,
          this->trigger_stop_insn_mask);
    printf("TracerV: Trigger enabled from start trigger instruction %x masked "
           "with %x, to end trigger instruction %x masked with %x\n",
           this->trigger_start_insn,
           this->trigger_start_insn_mask,
           this->trigger_stop_insn,
           this->trigger_stop_insn_mask);
  } else {
    // Writing 0 to triggerSelector permanently enables the trigger
    write(this->mmio_addrs->triggerSelector, this->trigger_selector);
    printf("TracerV: No trigger selected. Trigger enabled from %lu to %lu "
           "cycles\n",
           0ul,
           ULONG_MAX);
  }
  write(this->mmio_addrs->initDone, true);
}

size_t tracerv_t::process_tokens(int num_beats, int minimum_batch_beats) {
  size_t maximum_batch_bytes = num_beats * BridgeConstants::STREAM_WIDTH_BYTES;
  size_t minimum_batch_bytes =
      minimum_batch_beats * BridgeConstants::STREAM_WIDTH_BYTES;
  // TODO. as opt can mmap file and just load directly into it.
  alignas(4096)
      uint64_t OUTBUF[this->stream_depth * BridgeConstants::STREAM_WIDTH_BYTES];
  auto bytes_received = pull(this->stream_idx,
                             (char *)OUTBUF,
                             maximum_batch_bytes,
                             minimum_batch_bytes);
  // check that a tracefile exists (one is enough) since the manager
  // does not create a tracefile when trace_enable is disabled, but the
  // TracerV bridge still exists, and no tracefile is created by default.
  if (this->tracefiles.size() > 0) {
    if (this->human_readable || this->test_output) {
      for (int i = 0; i < (bytes_received / sizeof(uint64_t)); i += 8) {
        if (this->test_output) {
          fprintf(this->tracefile, "%016lx", OUTBUF[i + 7]);
          fprintf(this->tracefile, "%016lx", OUTBUF[i + 6]);
          fprintf(this->tracefile, "%016lx", OUTBUF[i + 5]);
          fprintf(this->tracefile, "%016lx", OUTBUF[i + 4]);
          fprintf(this->tracefile, "%016lx", OUTBUF[i + 3]);
          fprintf(this->tracefile, "%016lx", OUTBUF[i + 2]);
          fprintf(this->tracefile, "%016lx", OUTBUF[i + 1]);
          fprintf(this->tracefile, "%016lx\n", OUTBUF[i + 0]);
          // At least one valid instruction
        } else {
          if (this->userspace) {
            if (OUTBUF[i + 1] & valid_mask) {
              fprintf(this->tracefile,
                  "Cycle: %016" PRId64 " I%d: %016" PRIx64 " Inst: %016" PRIx64 " satp: %016" PRIx64 " priv: %016" PRIx64 "\n",
                  OUTBUF[i + 0],
                  0,
                  (uint64_t)((((int64_t)(OUTBUF[i + 1])) << 24) >> 24),
                  OUTBUF[i + 2],
                  OUTBUF[i + 3],
                  OUTBUF[i + 4]);
            }
          } else {
            for (int q = 0; q < max_core_ipc; q++) {
              if (OUTBUF[i + q + 1] & valid_mask) {
                fprintf(this->tracefile,
                    "Cycle: %016" PRId64 " I%d: %016" PRIx64 "\n",
                    OUTBUF[i + 0],
                    q,
                    OUTBUF[i + q + 1] & (~valid_mask));
              } else {
                break;
              }
            }
          }
        }
      }
    } else if (this->fireperf) {

      if (this->userspace) {
        for (int i = 0; i < (bytes_received / sizeof(uint64_t)); i += 8) {
          uint64_t cycle_internal = OUTBUF[i + 0];
        
          if (OUTBUF[i + 1] & valid_mask) {
            uint64_t iaddr =
              (uint64_t)((((int64_t)(OUTBUF[i + 1])) << 24) >> 24);
            struct token_t token;
            token.cycle_count = cycle_internal;
            token.iaddr = iaddr;
            token.inst = OUTBUF[i + 2];
            token.satp = OUTBUF[i + 3];
            token.priv = OUTBUF[i + 4];
            tracerv_t::matchAddInstruction(token, false);
#ifdef FIREPERF_LOGGER
            // fprintf(this->tracefile, "%016llx", iaddr);
            // fprintf(this->tracefile, "%016llx\n", cycle_internal);
            fprintf(fireperf_logger,
                "Cycle: %016" PRId64 " I%d: %016" PRIx64 " Inst: %016" PRIx64 " satp: %016" PRIx64 " priv: %016" PRIx64 "\n",
                OUTBUF[i + 0],
                0,
                (uint64_t)((((int64_t)(OUTBUF[i + 1])) << 24) >> 24),
                OUTBUF[i + 2],
                OUTBUF[i + 3],
                OUTBUF[i + 4]);
#endif // FIREPERF_LOGGER
          }
        } 
      } else {
        for (int i = 0; i < (bytes_received / sizeof(uint64_t)); i += 8) {
          uint64_t cycle_internal = OUTBUF[i + 0];

          for (int q = 0; q < max_core_ipc; q++) {
            if (OUTBUF[i + 1 + q] & valid_mask) {
              uint64_t iaddr =
                (uint64_t)((((int64_t)(OUTBUF[i + 1 + q])) << 24) >> 24);
              struct token_t token;
              token.cycle_count = cycle_internal;
              token.iaddr = iaddr;
              token.inst = 0;
              tracerv_t::matchAddInstruction(token, false);
#ifdef FIREPERF_LOGGER
              fprintf(this->tracefile, "%016llx", iaddr);
              fprintf(this->tracefile, "%016llx\n", cycle_internal);
#endif // FIREPERF_LOGGER
            }
          }
        } 
      }
    } else {
      for (int i = 0; i < (bytes_received / sizeof(uint64_t)); i += 8) {
        // this stores as raw binary. stored as little endian.
        // e.g. to get the same thing as the human readable above,
        // flip all the bytes in each 512-bit line.
        for (int q = 0; q < 8; q++) {
          fwrite(OUTBUF + (i + q), sizeof(uint64_t), 1, this->tracefile);
        }
      }
    }
  }
  return bytes_received;
  /*
  char *buf = (char *) malloc(sizeof(char) * BUFF_SIZE);
  if (!buf) {
    exit(1);
  }
  FILE *f = fopen("./stream.txt", "r");
  if (!f) {
    exit(1);
  }
  while (getline(&buf, (size_t *) &BUFF_SIZE, f) != -1) {
    struct token_t t;
    char *ptr = strtok(buf, " ");
    for (int i = 1; i < 10; i++) {
      ptr = strtok(NULL, " ");
      uint64_t data = strtoull(ptr, NULL, 16);
      switch (i) {
        case 1:
          t.cycle_count = data;
          break;
        case 3: 
          //
          //if (data >= 0x0000008000000000) {
            //data = data + 0xffffff0000000000;
          //}
          //
          t.iaddr = data;
          break;
        case 5: 
          t.inst = data;
          break;
        case 7:
          t.satp = data;
          break;
        case 9:
          t.priv = (uint8_t) data;
          break;
      }
    }
    t.bin = nullptr;
    t.instr_meta = nullptr;
    tracerv_t::matchAddInstruction(t, false);
  }
  return 0;
  */
}

void tracerv_t::tick() {
  if (this->trace_enabled) {
    process_tokens(this->stream_depth, this->stream_depth);
  }
}


// Pull in any remaining tokens and flush them to file
void tracerv_t::flush() {
  while (this->trace_enabled && (process_tokens(this->stream_depth, 0) > 0))
    ;
  while (this->retired_buffer.size() > 0) {
    struct token_t t;
    matchAddInstruction(t, true);
  }
}

void tracerv_t::matchAddInstruction(struct token_t token, bool flush) {
  if (!flush) {
    this->retired_buffer.push_back(token);
    if (this->retired_buffer.size() < BUFFER_SIZE) { // TODO
      return;
    }
  }
  struct token_t cur_token = this->retired_buffer.front();
  this->retired_buffer.pop_front();
  
  if (cur_token.iaddr == 0x1048e && cur_token.inst == 0x00000000f67ff0ef)
      fprintf(stderr, "Inside matchAddInstr I0: %016" PRIx64 " Inst: %016" PRIx64 "\n", cur_token.iaddr, cur_token.inst);
  
  if (tracerv_t::matchInstruction(cur_token)) {
  
    if (cur_token.iaddr == 0x1048e && cur_token.inst == 0x00000000f67ff0ef)
      fprintf(stderr, "Inside matchAddInstr post-matching I0: %016" PRIx64 " Inst: %016" PRIx64 "\n", cur_token.iaddr, cur_token.inst);
    
    this->trace_trackers[cur_token.bin->bin_name]->addInstruction(cur_token);
  
  } else {
    this->trace_trackers["misc"]->addInstruction(cur_token);
  }
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
bool tracerv_t::matchInstruction(struct token_t &token) {
   
  if (token.iaddr == 0x1048e && token.inst == 0x00000000f67ff0ef)
      fprintf(stderr, "Inside matchInstr I0: %016" PRIx64 " Inst: %016" PRIx64 "\n", token.iaddr, token.inst);
  
  if (token.iaddr >= this->kernel_objdump->baseaddr && 
                (token.iaddr - this->kernel_objdump->baseaddr) < this->kernel_objdump->progtext.size()) {
    /* Kernel */
    token.instr_meta = this->kernel_objdump->progtext[token.iaddr - this->kernel_objdump->baseaddr];
    token.bin = this->kernel_objdump;
    return true;
  } else {
    uint64_t computed_addr = token.iaddr - DRAM_ROOT;
    if (token.iaddr >= DRAM_ROOT && computed_addr < this->kernel_objdump->progtext.size()) {
      token.instr_meta = this->kernel_objdump->progtext[computed_addr];
      token.bin = this->kernel_objdump;
      return true;
    }
    /* user match by looking at buffer AND updating buffer */
    if (token.instr_meta != nullptr) { 
      // Check if the backpropogation was done correctly
      std::vector<struct bin_page_pair_t> possible_sites = this->offset_inst_to_page.at((token.iaddr%BYTES_PER_PAGE) >> 1)[token.inst];
      if (mapContains(possible_sites, token.page_base, token.bin)) {
        return true;
      } else {
        token.bin = nullptr;
        token.instr_meta = nullptr;
        token.page_base = 0;
      }
    }
    std::vector<struct bin_page_pair_t> possible_sites = this->offset_inst_to_page.at((token.iaddr%BYTES_PER_PAGE) >> 1)[token.inst];
    /* lookup in the cache for possible fast resolution and verify it was a hit by comparing the structs */
    /*
    if (this->satp_page_cache.count(token.satp) != 0 && this->satp_page_cache[token.satp].count(token.iaddr >> 12) != 0) {
      struct bin_page_pair_t hit = this->satp_page_cache[token.satp][token.iaddr >> 12];
      // possible bin is a hit but needs to be verified using the instruction bits
      for (auto const &item : possible_sites) {
        if (item.bin == hit.bin && item.page_base == hit.page_base) {
          token.instr_meta = hit.bin->progtext[token.iaddr%BYTES_PER_PAGE + hit.page_base - hit.bin->baseaddr];
          token.bin = hit.bin;
          return true;
        }
      }
    } 
    */
    /* regular matching if not found in cache or found some conflicting entries  and */
    if (possible_sites.size() == 0) {
      /* failed to find any matching binary, USERSPACE ALL */
      if (token.iaddr == 0x1048e && token.inst == 0x00000000f67ff0ef)
        fprintf(stderr, "No possible sites found");  
      
      return false;
    } 
    /*
    else if (possible_sites.size() == 1) {
      // best case, found the unique match
      token.instr_meta = possible_sites.at(0).bin->progtext[token.iaddr%BYTES_PER_PAGE + possible_sites.at(0).page_base - possible_sites.at(0).bin->baseaddr];
      if (!token.instr_meta) {
        printf("WARNING: instr object is null.");
      }
      token.bin = possible_sites.at(0).bin;


      return true;
    } 
    */
    else {
      /* no unique possible_sites were found */
      std::vector<struct token_t *> matching_vec;
      for (auto it = this->retired_buffer.begin(); matching_vec.size() < MATCHING_DEPTH && it != this->retired_buffer.end(); ++it) { 
        if (filterBuffer(&(*it), token.satp)) {
          matching_vec.push_back(&(*it));
        }
      }
      std::vector<struct bin_page_pair_t> matched_sites;
      for (auto const &s : possible_sites) {
        for (size_t i = 0; i < matching_vec.size(); i++) {
          struct token_t *m = matching_vec.at(i);
          uint64_t p = ((m->iaddr >> 12) << 12) - ((token.iaddr >> 12) << 12) + s.page_base;
          std::vector<struct bin_page_pair_t> v = this->offset_inst_to_page[(m->iaddr%BYTES_PER_PAGE) >> 1][m->inst];
          if(!tracerv_t::mapContains(v, p, s.bin)) {
            break;
          }
          if (i == matching_vec.size() - 1) {
            matched_sites.push_back(s);
          }
        }
      }
      // check the number of matches sites; check if matched_sites binaries are the same in case there are more than 1
      if (matched_sites.size() == 0)  {
        if (token.iaddr == 0x1048e && token.inst == 0x00000000f67ff0ef)
          fprintf(stderr, "matched sites ended up as 0");  
        // failed to find any matching
        return false;
      } else if (matched_sites.size() == 1) {
        token.instr_meta = matched_sites.at(0).bin->progtext[token.iaddr%BYTES_PER_PAGE + matched_sites.at(0).page_base - matched_sites.at(0).bin->baseaddr];
        token.bin = matched_sites.at(0).bin;
        if (!token.instr_meta) {
          printf("WARNING: instr object is null.");
        }
        token.page_base = matched_sites.at(0).page_base;
        // back propogation logic
        for (auto &t : this->retired_buffer) {
          if (t.satp == token.satp && t.priv == 0) {
            uint64_t potential_page_base = ((t.iaddr >> 12) << 12) + token.page_base - ((token.iaddr >> 12) << 12);
            uint64_t index = (t.iaddr % BYTES_PER_PAGE) + potential_page_base - token.bin->baseaddr; 
            if (index < token.bin->progtext.size()) {
              t.instr_meta = token.bin->progtext[index];
              t.bin = token.bin;
              t.page_base = potential_page_base;
            } 
          }
          /*
          t->bin = token.bin;
          t->page_base = ((t->iaddr >> 12) << 12) - ((token.iaddr >> 12) << 12) + token.page_base; 
          t->instr_meta = token.bin->progtext[t->iaddr % BYTES_PER_PAGE + t->page_base - token.bin->baseaddr];
          */
        }
        return true;
      } else {
        if (token.iaddr == 0x1048e && token.inst == 0x00000000f67ff0ef)
          fprintf(stderr, "matched sites size = %d", matched_sites.size());  
        
        for (size_t i = 1; i < matched_sites.size(); i++) {  
          if (matched_sites.at(0).bin != matched_sites.at(i).bin) { // FIXME
            return false;
          } 
        }
        //token.instr_meta = new Instr(); 
        //token.instr_meta->function_name.assign(matched_sites.at(0).bin->bin_name); 
        //token.bin = matched_sites.at(0).bin;
        return false; // FIXME: throw to misc for now, but change later
      }
    }
  }
}

bool tracerv_t::mapContains(std::vector <struct bin_page_pair_t> v, uint64_t page_base, ObjdumpedBinary *bin) {
  for (auto const &el : v) {
    if (el.bin == bin && el.page_base == page_base) {
      return true;
    }
  }
  return false;
}

bool tracerv_t::filterBuffer(struct token_t* token, uint64_t satp) {
  if ((token->iaddr >= this->kernel_objdump->baseaddr 
    && (token->iaddr - this->kernel_objdump->baseaddr) < this->kernel_objdump->progtext.size())
    || satp != token->satp) 
   return false;
  return true; 
}
#endif // TRACERVBRIDGEMODULE_struct_guard

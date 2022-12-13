// See LICENSE for license details
#ifndef __TRACERV_H
#define __TRACERV_H

#include "bridges/bridge_driver.h"
#include "bridges/clock_info.h"
#include "bridges/tracerv/trace_tracker.h"
#include "bridges/tracerv/tracerv_processing.h"
#include <vector>
#include <deque>
#include <inttypes.h>
#include <map>
#include <filesystem>
#include "tracerv/trace_tracker.h"

#ifdef TRACERVBRIDGEMODULE_struct_guard

// Bridge Driver Instantiation Template
#define INSTANTIATE_TRACERV(FUNC, IDX)                                         \
  TRACERVBRIDGEMODULE_##IDX##_substruct_create;                                \
  FUNC(new tracerv_t(this,                                                     \
                     args,                                                     \
                     TRACERVBRIDGEMODULE_##IDX##_substruct,                    \
                     TRACERVBRIDGEMODULE_##IDX##_to_cpu_stream_idx,            \
                     TRACERVBRIDGEMODULE_##IDX##_to_cpu_stream_depth,          \
                     TRACERVBRIDGEMODULE_##IDX##_max_core_ipc,                 \
                     TRACERVBRIDGEMODULE_##IDX##_clock_domain_name,            \
                     TRACERVBRIDGEMODULE_##IDX##_clock_multiplier,             \
                     TRACERVBRIDGEMODULE_##IDX##_clock_divisor,                \
                     IDX));

class tracerv_t : public bridge_driver_t

{
public:

  // Add matching depth as arg	
  tracerv_t(simif_t *sim,
          std::vector<std::string> &args,
          TRACERVBRIDGEMODULE_struct *mmio_addrs,
          const int stream_idx,
          const int stream_depth,
          const unsigned int max_core_ipc,
          const char *const clock_domain_name,
          const unsigned int clock_multiplier,
          const unsigned int clock_divisor,
          int tracerno, 
          int matching_depth = 3, uint8_t userspace = 1);

  ~tracerv_t();

  virtual void init();
  virtual void tick();
  virtual bool terminate() { return false; }
  virtual int exit_code() { return 0; }
  virtual void finish() { flush(); };
  
	virtual size_t process_tokens(int num_beats, int minium_batch_beats);

private:

	TRACERVBRIDGEMODULE_struct *mmio_addrs;
	const int stream_idx;
	const int stream_depth;
	const int max_core_ipc;
	ClockInfo clock_info;		
	
	bool buffer_flush_mode;
  // matching specific data structures
  const uint32_t BUFFER_SIZE = 2048;
  uint8_t MATCHING_DEPTH;
  uint8_t userspace;
  std::deque<struct token_t> retired_buffer;
  ObjdumpedBinary* kernel_objdump; 
  std::map<uint64_t, std::map<uint64_t, struct bin_page_pair_t>> satp_page_cache;
  std::vector<std::map<uint64_t, std::vector<struct bin_page_pair_t>>> offset_inst_to_page;
  bool filterBuffer(struct token_t *, uint64_t);
  bool mapContains(std::vector<struct bin_page_pair_t>, uint64_t, ObjdumpedBinary *);
  void matchAddInstruction(struct token_t, bool);

  //const int stream_depth;

  FILE * tracefile; //Dummy for non-fireperf modes, does not work currently //FIXME
  
	std::map<std::string, FILE *> tracefiles;
  uint64_t cur_cycle;
  uint64_t trace_trigger_start, trace_trigger_end;
  uint32_t trigger_start_insn = 0;
  uint32_t trigger_start_insn_mask = 0;
  uint32_t trigger_stop_insn = 0;
  uint32_t trigger_stop_insn_mask = 0;
  uint32_t trigger_selector;
  uint64_t trigger_start_pc = 0;
  uint64_t trigger_stop_pc = 0;

  // Kernel has a TraceTracker, each userspace program has a TraceTracker object as well
  std::map<std::string, TraceTracker *> trace_trackers;

  bool human_readable = false;
  // If no filename is provided, the instruction trace is not collected
  // and the bridge drops all tokens to improve FMR
  bool trace_enabled = true;
  // Used in unit testing to check TracerV is correctly pulling instuctions off
  // the target
  bool test_output = false;
  long dma_addr;
  std::string tracefilename;
  std::string dwarf_dir_name;
  std::string dwarf_file_name;
  bool fireperf = false;

  bool matchInstruction(struct token_t &token);
  int beats_available_stable();
  void copyFile(FILE*, FILE*);
  void flush();
};
#endif // TRACERVBRIDGEMODULE_struct_guard

#endif // __TRACERV_H

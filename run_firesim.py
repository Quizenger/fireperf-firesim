import os
import pandas as pd
import yaml
import subprocess
from pathlib import Path
import logging

# Paths
CONFIG_PATH = "deploy/config_runtime.yaml"
OUTPUT_DIR = os.path.expanduser("~/FIRESIM_RUNS_DIR/sim_slot_0")
CSV_OUTPUT = "simulation_results.csv"
LOG_FILE = "simulation_script.log"
NUM_ITERS = 3

# Set up logging
logging.basicConfig(
    filename=LOG_FILE,
    filemode='w',  # Overwrite the log file on each run
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)

# Configurations to iterate over
DEFAULT_HW_CONFIGS = [
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_singlecore_4GB_no_nic",
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_bridge_singlecore_4GB_no_nic",
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_singlecore_4GB_no_nic_10",
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_bridge_singlecore_4GB_no_nic_10",
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_singlecore_4GB_no_nic_100",
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_bridge_singlecore_4GB_no_nic_100"
    "xilinx_vcu118_firesim_smallboom_null_prefetcher_singlecore_4GB_no_nic_10",
    "xilinx_vcu118_firesim_smallboom_null_prefetcher_singlecore_4GB_no_nic_50",
    "xilinx_vcu118_firesim_smallboom_null_prefetcher_singlecore_4GB_no_nic_100"
]
WORKLOAD_NAMES = [
    "coremark.json"
#    "bare-gcd.json",
#    "bare-gcd-10.json",
#    "bare-gcd-50.json",
#    "bare-gcd-100.json"
]

# Extract values from UART log
def parse_uart_log(log_path):
    results = {}
    with open(log_path, 'r') as file:
        # Read only the last 10 lines
        last_lines = file.readlines()[-10:]
        for line in last_lines:
            if "Wallclock Time Elapsed" in line:
                results['Wallclock Time Elapsed (s)'] = line.split(":")[-1].strip()
            elif "Host Cycles Executed" in line:
                results['Host Cycles Executed'] = line.split(":")[-1].strip()
            elif "Host Frequency" in line:
                results['Host Frequency (MHz)'] = line.split(":")[-1].strip()
            elif "Target Cycles Emulated" in line:
                results['Target Cycles Emulated'] = line.split(":")[-1].strip()
            elif "Effective Target Frequency" in line:
                results['Effective Target Frequency (MHz)'] = line.split(":")[-1].strip()
            elif "FMR" in line:
                results['FMR'] = line.split(":")[-1].strip()
    return results

# Modify config_runtime.yaml
def update_yaml_file(hw_config, workload_name):
    with open(CONFIG_PATH, 'r') as file:
        config = yaml.safe_load(file)
    
    config['target_config']['default_hw_config'] = hw_config
    config['workload']['workload_name'] = workload_name
    
    with open(CONFIG_PATH, 'w') as file:
        yaml.dump(config, file)
    logging.info(f"Updated config_runtime.yaml with HW Config: {hw_config}, Workload: {workload_name}")

def add_built_hwdb_entries():
    destination_file = "/home/raghavgupta/hybrid-sim/chipyard/sims/firesim-staging/sample_config_hwdb.yaml"
    for hw_config in DEFAULT_HW_CONFIGS:
        source_file = f"deploy/built-hwdb-entries/{hw_config}"
        try:
            # Ensure the source file exists
            if not os.path.exists(source_file):
                raise FileNotFoundError(f"Source file '{source_file}' not found.")

            # Read the source file
            with open(source_file, 'r') as src:
                content = src.read()

            # Append the content to the destination file
            with open(destination_file, 'a') as dest:
                dest.write(content)

            logging.info(f"Contents of '{source_file}' successfully appended to '{destination_file}'.")

        except FileNotFoundError as e:
            logging.error(e)

        except Exception as e:
            logging.error(f"An error occurred: {e}")

        
        
# Main process
def main():
    
    add_built_hwdb_entries()

    results = []
    for hw_config in DEFAULT_HW_CONFIGS:
        for workload_name in WORKLOAD_NAMES:
            for run in range(NUM_ITERS):
                logging.info(f"Running hw_config={hw_config}, workload={workload_name}, run={run+1}/10")
                
                # Update config file
                update_yaml_file(hw_config, workload_name)
                
                # Run the simulation
                cmd = (
                    "firesim infrasetup -a /home/raghavgupta/hybrid-sim/chipyard/sims/firesim-staging/sample_config_hwdb.yaml "
                    "-r /home/raghavgupta/hybrid-sim/chipyard/sims/firesim-staging/sample_config_build_recipes.yaml && "
                    "firesim runworkload -a /home/raghavgupta/hybrid-sim/chipyard/sims/firesim-staging/sample_config_hwdb.yaml "
                    "-r /home/raghavgupta/hybrid-sim/chipyard/sims/firesim-staging/sample_config_build_recipes.yaml"
                )

                try:
                    subprocess.run(cmd, shell=True, check=True)
                except subprocess.CalledProcessError as e:
                    # Log the error but don't skip this iteration
                    logging.warning(f"Ignored error during simulation command: {e}")
                finally:
                    # Continue parsing logs and saving results even if the command errors out
                    uart_log = Path(OUTPUT_DIR) / "uartlog"
                    if uart_log.exists():
                        run_results = parse_uart_log(uart_log)
                        run_results.update({
                            "HW Config": hw_config,
                            "Workload": workload_name,
                            "Run": run + 1
                        })
                        results.append(run_results)
                        logging.info(f"Parsed UART log for run {run+1}/10")
                    else:
                        logging.warning(f"UART log not found for run {run+1}")

    # Save results to CSV
    df = pd.DataFrame(results)
    df.to_csv(CSV_OUTPUT, index=False)
    logging.info(f"Results saved to {CSV_OUTPUT}")

if __name__ == "__main__":
    logging.info("Simulation script started")
    main()
    logging.info("Simulation script completed")


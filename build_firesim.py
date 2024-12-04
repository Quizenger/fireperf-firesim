import subprocess
import yaml

# List of values to iterate over
values = [
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_singlecore_4GB_no_nic_1",
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_bridge_singlecore_4GB_no_nic_1",
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_singlecore_4GB_no_nic_10",
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_bridge_singlecore_4GB_no_nic_10",
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_singlecore_4GB_no_nic_100",
#    "xilinx_vcu118_firesim_smallboom_gcd_tl_bridge_singlecore_4GB_no_nic_100"
    "xilinx_vcu118_firesim_smallboom_gcd_tl_singlecore_4GB_no_nic_50",
    "xilinx_vcu118_firesim_smallboom_gcd_tl_bridge_singlecore_4GB_no_nic_50"
]

# Path to the YAML file
yaml_file_path = "deploy/config_build.yaml"

# Command arguments
arg_hwdb = "/home/raghavgupta/hybrid-sim/chipyard/sims/firesim-staging/sample_config_hwdb.yaml"
arg_build_recipes = "/home/raghavgupta/hybrid-sim/chipyard/sims/firesim-staging/sample_config_build_recipes.yaml"

def update_yaml_and_build(value):
    """Update the YAML file and run the build command."""
    
    print(f"Loading yaml file", flush=True)
    # Load the existing YAML file
    with open(yaml_file_path, "r") as f:
        config = yaml.safe_load(f)
    
    # Update the specific entry in the YAML file
    config['builds_to_run'][0] = value

    print(f"Updating yaml file", flush=True)
    # Save the updated YAML file
    with open(yaml_file_path, "w") as f:
        yaml.dump(config, f)

    # Run the build command
    print(f"Building for: {value}", flush=True)
    result = subprocess.run(
        ["firesim", "buildbitstream", 
         "-a", arg_hwdb, 
         "-r", arg_build_recipes]
    )

    # Log output and errors if any
    if result.returncode == 0:
        print(f"Build succeeded for: {value}\n", flush=True)
        # print(result.stdout)  # Print output if needed
    else:
        print(f"Build failed for: {value}", flush=True)

# Loop through each value and perform the update-build sequence
for value in values:
    update_yaml_and_build(value)


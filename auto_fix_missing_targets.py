import re
import os
import subprocess
import struct
from capstone import *

base_addr = 0x100000
elf_offset = 0x1000
game_iso = 'sho/original_iso/SLES_551.47'
csv_file = 'sho/sho.csv'

def find_func_size(target_addr):
    f = open(game_iso, 'rb').read()
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS64 + CS_MODE_LITTLE_ENDIAN)
    addr = target_addr
    file_off = elf_offset + (target_addr - base_addr)
    for i in md.disasm(f[file_off : file_off + 0x4000], target_addr):
        if i.mnemonic == 'jr' and i.op_str == '$ra':
            # return address of the delay slot + 4
            return (i.address + 8) - target_addr
    return 0

def run_game():
    cmd = "timeout -s KILL 15 ./build-run/ps2xRuntime/ps2EntryRunner ./sho/original_iso/SLES_551.47"
    print(f"Running game: {cmd}")
    res = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    out = res.stdout + res.stderr
    
    match = re.search(r'\[guest-branch:missing-target\].*?target=(0x[0-9a-fA-F]+)', out)
    if match:
        return int(match.group(1), 16)
    return None

def rebuild():
    print("Recompiling...")
    subprocess.run("./build-run/ps2xRecomp/ps2_recomp sho/config_from_analyzer.toml", shell=True, check=True)
    print("Building...")
    subprocess.run("cmake --build build-run --target ps2EntryRunner -j 8", shell=True, check=True)

for iteration in range(15):
    print(f"--- Iteration {iteration+1} ---")
    target = run_game()
    if target is None:
        print("No missing target found! Game might be running.")
        break
        
    print(f"Found missing target: {hex(target)}")
    
    # check if already in csv
    csv_content = open(csv_file).read()
    if f"0x{target:08X}" in csv_content or f"{target:x}" in csv_content.lower():
        print("Target is already in CSV. We will rebuild and try again (maybe we didn't rebuild yet).")
        rebuild()
        
        target2 = run_game()
        if target2 == target:
            print("ERROR: Target is STILL missing after rebuild! Aborting.")
            break
        target = target2
        if target is None:
            break
            
    size = find_func_size(target)
    if size == 0:
        print("Could not find end of function.")
        break
        
    end_addr = target + size
    csv_line = f"FUN_{target:08x},0x{target:08X},0x{end_addr:08X},{size}"
    print(f"Adding to CSV: {csv_line}")
    with open(csv_file, 'a') as f:
        f.write(csv_line + "\n")
        
    rebuild()

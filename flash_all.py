#!/usr/bin/env python3
"""
Script to flash the ESP32-Doom project with all partitions
"""
import os
import sys
import subprocess
import argparse

def run_command(cmd, description):
    """Run a command and handle errors"""
    print(f"\n{description}...")
    print(f"Command: {' '.join(cmd)}")
    
    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        print(f"✓ {description} completed successfully")
        return True
    except subprocess.CalledProcessError as e:
        print(f"✗ Error during {description}: {e}")
        print(f"stdout: {e.stdout}")
        print(f"stderr: {e.stderr}")
        return False

def flash_all():
    """Flash all partitions to the ESP32"""
    
    # Get ESP-IDF environment variables
    espport = os.environ.get('ESPPORT', '')
    espbaud = os.environ.get('ESPBAUD', '115200')
    espflashmode = os.environ.get('ESPFLASHMODE', 'dio')
    espflashfreq = os.environ.get('ESPFLASHFREQ', '40m')
    espflashsize = os.environ.get('ESPFLASHSIZE', '4MB')
    
    if not espport:
        print("Error: ESPPORT environment variable not set!")
        print("Please set ESPPORT to your device port (e.g., /dev/ttyUSB0)")
        print("You can also set it via: export ESPPORT=/dev/ttyUSB0")
        return False
    
    # Check if build files exist
    bootloader_bin = "build/bootloader/bootloader.bin"
    partition_table_bin = "build/partition_table/partition-table.bin"
    app_bin = "build/esp32-doom.bin"
    storage_bin = "build/storage.bin"
    wad_bin = "build/wad_partition.bin"
    
    required_files = [
        (bootloader_bin, "Bootloader"),
        (partition_table_bin, "Partition table"),
        (app_bin, "Application"),
        (storage_bin, "SPIFFS storage"),
        (wad_bin, "WAD file")
    ]
    
    for file_path, description in required_files:
        if not os.path.exists(file_path):
            print(f"Error: {description} file not found: {file_path}")
            print("Please run 'idf.py build' first")
            return False
    
    # Build the esptool command for flashing all partitions
    cmd = [
        'esptool.py',
        '--chip', 'esp32',
        '--port', espport,
        '--baud', espbaud,
        'write_flash',
        '--flash_mode', espflashmode,
        '--flash_freq', espflashfreq,
        '--flash_size', espflashsize,
        '0x1000', bootloader_bin,      # Bootloader at 0x1000
        '0x8000', partition_table_bin,  # Partition table at 0x8000
        '0x10000', app_bin,            # Application at 0x10000
        '0x188000', wad_bin,           # WAD file at 0x188000 (wad partition)
        '0x388000', storage_bin        # SPIFFS at 0x388000 (storage partition)
    ]
    
    return run_command(cmd, "Flashing all partitions")

def main():
    parser = argparse.ArgumentParser(description='Flash ESP32-Doom with all partitions')
    parser.add_argument('--port', '-p', help='ESP32 device port (overrides ESPPORT env var)')
    
    args = parser.parse_args()
    
    # Override ESPPORT if provided as argument
    if args.port:
        os.environ['ESPPORT'] = args.port
    
    print("ESP32-Doom Flash Script")
    print("=======================")
    
    success = flash_all()
    
    if success:
        print("\n✓ All partitions flashed successfully!")
        print("The ESP32 should now boot with:")
        print("- Application firmware")
        print("- WAD file in the wad partition")
        print("- index.html in the SPIFFS storage partition")
    else:
        print("\n✗ Flashing failed!")
        sys.exit(1)

if __name__ == "__main__":
    main() 
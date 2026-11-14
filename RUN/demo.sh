#!/usr/bin/env bash
# Interactive Terminal Demo Runner for Batch Process Monitor
set -e

cd "$(dirname "$0")/.."

echo "============================================="
echo "   Batch Process Monitor - Terminal Runner   "
echo "============================================="
echo ""

# Ensure executables are built
if [[ ! -f "build/batch_monitor" || ! -f "build/sleepy" ]]; then
    echo ">> Compiling binaries..."
    make
    echo ""
fi

echo "Select a demonstration input to run:"
echo "  1) Fast 5-Second Demo (RUN/INPUTS/input_quick_demo.txt)"
echo "  2) Full 20-Second Project Sheet Demo (RUN/INPUTS/input_sheet.txt)"
echo "  3) Simple Command Format Demo (RUN/INPUTS/input_simple.txt)"
echo "  4) Custom Input File"
echo "  5) Exit"
echo ""

read -rp "Enter choice [1-5]: " choice

case "$choice" in
    1)
        echo -e "\n>> Running Fast 5-Second Demo:"
        cat RUN/INPUTS/input_quick_demo.txt
        echo -e "\n=============================================\n"
        ./build/batch_monitor RUN/INPUTS/input_quick_demo.txt
        ;;
    2)
        echo -e "\n>> Running Full 20-Second Project Sheet Demo:"
        cat RUN/INPUTS/input_sheet.txt
        echo -e "\n=============================================\n"
        ./build/batch_monitor RUN/INPUTS/input_sheet.txt
        ;;
    3)
        echo -e "\n>> Running Simple Command Format Demo:"
        cat RUN/INPUTS/input_simple.txt
        echo -e "\n=============================================\n"
        ./build/batch_monitor RUN/INPUTS/input_simple.txt
        ;;
    4)
        read -rp "Enter path to custom input file: " custom_file
        if [[ -f "$custom_file" ]]; then
            echo -e "\n>> Running $custom_file:\n"
            ./build/batch_monitor "$custom_file"
        else
            echo "Error: file '$custom_file' not found."
            exit 1
        fi
        ;;
    5)
        echo "Exiting."
        exit 0
        ;;
    *)
        echo "Invalid option. Exiting."
        exit 1
        ;;
esac

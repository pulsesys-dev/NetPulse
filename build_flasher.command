#!/bin/bash
cd "$(dirname "$0")"
python3 build_flasher.py "$@"
echo
read -p "Press Enter to close..."

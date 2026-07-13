#!/bin/bash
# Starts the mr-greeny backend. Run from anywhere:
#   ./run.sh
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

if [ ! -d .venv ]; then
    echo "No .venv found -- run this first:"
    echo "  python3 -m venv .venv && source .venv/bin/activate && pip install -r requirements.txt"
    exit 1
fi

source .venv/bin/activate
exec uvicorn app:app --host 0.0.0.0 --port 8000

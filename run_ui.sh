#!/bin/bash
# Build the UI and serve it with the backend on http://127.0.0.1:8000
#   ./run_ui.sh          talk to the board over USB (auto-connect)
#   ./run_ui.sh --sim    start in simulator mode (no hardware)
set -e
cd "$(dirname "$0")"
( cd frontend && { [ -d node_modules ] || npm ci; } && npm run build )
cd backend
[ "$1" = "--sim" ] && export BLDC_SIM=1
echo "UI: http://127.0.0.1:8000"
exec uv run uvicorn app.main:app --host 127.0.0.1 --port 8000

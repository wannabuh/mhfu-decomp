#!/bin/sh
# A local Python environment for the tools: tools/.venv, with tools/requirements.txt installed.
set -e
cd "$(dirname "$0")"
[ -d .venv ] || python3 -m venv .venv
.venv/bin/pip install --quiet --upgrade pip
.venv/bin/pip install --quiet -r requirements.txt
echo "tools/.venv ready: $(.venv/bin/python -c 'import rabbitizer; print("rabbitizer", rabbitizer.__version__)')"

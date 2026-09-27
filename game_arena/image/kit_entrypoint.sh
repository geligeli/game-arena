#!/bin/sh
# To stderr: stdout is the command's, and `arena_cli mcp`'s is a protocol.
arena_cli init >&2
exec "$@"

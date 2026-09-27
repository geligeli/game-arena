#!/bin/sh
# What a kit container does before its command. To stderr: stdout is the
# command's, and `arena_cli mcp`'s is a protocol.
arena_cli init >&2
exec "$@"

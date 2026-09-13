exports_files(["requirements.txt"])

# The MCP server a kit runs, with the requirements the pip extension reads.
filegroup(
    name = "kit_surface_files",
    srcs = glob(["**"]) + ["//mcp_servers/arena_mcp:kit_surface_files"],
    visibility = ["//visibility:public"],
)

#ifndef GAME_ARENA_GAME_ARENA_COMMON_KV_OPTIONS_KV_OPTIONS_H
#define GAME_ARENA_GAME_ARENA_COMMON_KV_OPTIONS_KV_OPTIONS_H

// "k=v,k2=v2" <-> map, for settings that ride on a command line.
//
// Both directions live together because they are one format: the worker builds
// the string and the referee parses it, in different processes built from
// different packages, and a format that is written in one place and read in
// another is a format that drifts.
//
// Keys and values may not contain ',' or '='. That is checked at problem-config
// load time (IsValidKey / IsValidValue) so a bad setting is a startup error
// with a clear message rather than a silently mangled flag on a worker.

#include <map>
#include <string>
#include <string_view>

namespace kv_options {

// Parses "k=v,k2=v2". Ignores empty entries and entries with no '='; a
// duplicate key keeps the last value. Never fails: these bytes come off a
// command line built by another process, and a referee that refused to start
// over one unparsable setting would take an order down with it.
auto Parse(std::string_view text) -> std::map<std::string, std::string>;

// Renders a map as "k=v,k2=v2", keys in sorted order so the result is stable
// (a work order's argv ends up in test expectations and in logs).
auto Format(const std::map<std::string, std::string> &options) -> std::string;

auto IsValidKey(std::string_view key) -> bool;
auto IsValidValue(std::string_view value) -> bool;

}  // namespace kv_options

#endif  // GAME_ARENA_GAME_ARENA_COMMON_KV_OPTIONS_KV_OPTIONS_H

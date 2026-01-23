#!/usr/bin/awk -f
# Minimal YAML subset parser for GoofyDeck paging.
# Supported (subset) structure:
#   system_buttons: { $page.back: {position: N}, ... }
#   presets: { preset_name: { key: value, ... }, ... }
#   pages: { page_name: { buttons: [ {name:.., icon:.., presets:[..]}, ... ] } }
#
# Output (tab-separated):
#   SYS <action> <position>
#   PRESET <preset> <key> <value>
#   PAGE <page>
#   BTN <page> <index> <name> <icon> <preset_csv>
#
# Notes:
# - Strings may be quoted with single/double quotes; quotes are stripped.
# - Comments (# ...) are stripped when preceded by whitespace.

function ltrim(s) { sub(/^[ \t\r\n]+/, "", s); return s }
function rtrim(s) { sub(/[ \t\r\n]+$/, "", s); return s }
function trim(s) { return rtrim(ltrim(s)) }
function strip_quotes(s) {
  s = trim(s)
  if ((s ~ /^".*"$/) || (s ~ /^'.*'$/)) {
    return substr(s, 2, length(s)-2)
  }
  return s
}
function indent_of(line,   m) {
  match(line, /^[ ]*/)
  return RLENGTH
}
function strip_inline_comment(s,   out, i, c, prev) {
  # Remove " #..." comments (simple heuristic; does not fully parse YAML quoting).
  out = ""
  prev = ""
  for (i = 1; i <= length(s); i++) {
    c = substr(s, i, 1)
    if (c == "#" && (prev == " " || prev == "\t")) {
      break
    }
    out = out c
    prev = c
  }
  return rtrim(out)
}

BEGIN {
  OFS = "\t"
  section = ""
  preset = ""
  page = ""
  in_buttons = 0
  in_btn_presets = 0
  btn_idx = 0
  last_sys = ""
}

{
  raw = $0
  gsub(/\r$/, "", raw)
  line = strip_inline_comment(raw)
  if (trim(line) == "") next

  ind = indent_of(line)
  t = trim(line)

  if (t == "system_buttons:") { section="system_buttons"; preset=""; page=""; in_buttons=0; next }
  if (t == "presets:") { section="presets"; preset=""; page=""; in_buttons=0; next }
  if (t == "pages:") { section="pages"; preset=""; page=""; in_buttons=0; next }

  if (section == "system_buttons") {
    if (ind == 2 && match(t, /^([^:]+):$/, m)) {
      last_sys = strip_quotes(m[1])
      next
    }
    if (ind >= 4 && last_sys != "" && match(t, /^position:[ ]*([0-9]+)$/, m)) {
      print "SYS", last_sys, m[1]
      next
    }
  }

  if (section == "presets") {
    if (ind == 2 && match(t, /^([^:]+):$/, m)) {
      preset = strip_quotes(m[1])
      next
    }
    if (ind >= 4 && preset != "" && match(t, /^([^:]+):[ ]*(.*)$/, m)) {
      key = strip_quotes(m[1])
      val = strip_quotes(m[2])
      print "PRESET", preset, key, val
      next
    }
  }

  if (section == "pages") {
    if (ind == 2 && match(t, /^([^:]+):$/, m)) {
      page = strip_quotes(m[1])
      print "PAGE", page
      in_buttons = 0
      in_btn_presets = 0
      btn_idx = 0
      next
    }
    if (page != "" && ind == 4 && t == "buttons:") {
      in_buttons = 1
      in_btn_presets = 0
      btn_idx = 0
      next
    }
    if (in_buttons) {
      if (ind == 6 && match(t, /^-[ ]*(.*)$/, m)) {
        btn_idx++
        btn_name[page,btn_idx] = ""
        btn_icon[page,btn_idx] = ""
        btn_presets[page,btn_idx] = ""
        in_btn_presets = 0
        rest = trim(m[1])
        if (match(rest, /^name:[ ]*(.*)$/, mm)) btn_name[page,btn_idx] = strip_quotes(mm[1])
        if (match(rest, /^icon:[ ]*(.*)$/, mm)) btn_icon[page,btn_idx] = strip_quotes(mm[1])
        next
      }
      if (btn_idx > 0 && ind == 8 && match(t, /^name:[ ]*(.*)$/, m)) {
        btn_name[page,btn_idx] = strip_quotes(m[1])
        next
      }
      if (btn_idx > 0 && ind == 8 && match(t, /^icon:[ ]*(.*)$/, m)) {
        btn_icon[page,btn_idx] = strip_quotes(m[1])
        next
      }
      if (btn_idx > 0 && ind == 8 && t == "presets:") {
        in_btn_presets = 1
        next
      }
      if (btn_idx > 0 && ind == 8 && match(t, /^presets:[ ]*\[(.*)\][ ]*$/, m)) {
        in_btn_presets = 0
        list = m[1]
        gsub(/,/, "\n", list)
        split(list, arr, "\n")
        for (ai in arr) {
          v = strip_quotes(arr[ai])
          v = trim(v)
          if (v == "") continue
          if (btn_presets[page,btn_idx] == "") btn_presets[page,btn_idx] = v
          else btn_presets[page,btn_idx] = btn_presets[page,btn_idx] "," v
        }
        next
      }
      if (btn_idx > 0 && in_btn_presets && ind == 10 && match(t, /^-[ ]*(.*)$/, m)) {
        p = strip_quotes(m[1])
        if (btn_presets[page,btn_idx] == "") btn_presets[page,btn_idx] = p
        else btn_presets[page,btn_idx] = btn_presets[page,btn_idx] "," p
        next
      }
      if (btn_idx > 0 && ind <= 8) {
        in_btn_presets = 0
      }
    }
  }
}

END {
  # Emit BTN records in page order (as encountered), button order (1..N).
  # We don't store page order explicitly; we re-parse stored PAGE lines would be complex,
  # so just emit in hash iteration order? Instead, rely on immediate printing while parsing:
  # We'll do a second pass by tracking max index per page while parsing and print at end.
  # Track max per page:
  for (k in btn_name) {
    split(k, parts, SUBSEP)
    p = parts[1]
    i = parts[2] + 0
    if (i > max_btn[p]) max_btn[p] = i
  }
  for (p in max_btn) {
    for (i = 1; i <= max_btn[p]; i++) {
      print "BTN", p, i, btn_name[p,i], btn_icon[p,i], btn_presets[p,i]
    }
  }
}

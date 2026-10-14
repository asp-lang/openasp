#!/bin/sh
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

set -eu
program=$1
project=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
printf '%s\n' '<% name = Request.QueryString("name") %><h1><%=Server.HTMLEncode(name)%></h1>' > "$tmp/index.asp"
output=$("$program" "$tmp/index.asp" --root "$tmp" --query 'name=Open%20ASP')
test "$output" = '<h1>Open ASP</h1>'

reference=${OPENASP_REFERENCE_ROOT:-"$project/tests"}
if test -d "$reference/fixtures/reference"; then
    fixture_output=$("$program" "$reference/fixtures/reference/index.asp" \
        --root "$reference/fixtures/reference" --query 'name=OpenASP')
    printf '%s' "$fixture_output" | grep -q '<div>header-from-include</div>'
    printf '%s' "$fixture_output" | grep -q '<div>site=egret-asp</div>'
    printf '%s' "$fixture_output" | grep -q '<div>session=OpenASP</div>'

    bnf_output=$("$program" \
        "$reference/fixtures/reference/vbscript_bnf_coverage.asp" \
        --root "$reference/fixtures/reference" | tr -d '\r\n')
    test "$bnf_output" = \
        '5|8|16|100|25|0|0|True|True|2020|7|1|2|item3|box|9|FalseTrueFalseTrue|TrueFalseTrueFalse|10|11'

    exit_output=$("$program" \
        "$reference/fixtures/reference/nested_loop_exit_copyback.asp" \
        --root "$reference/fixtures/reference" | tr -d '\r\n')
    test "$exit_output" = \
        'sub:10|for:11|selected|break:1|outer|True|end'

    receiver_output=$("$program" \
        "$reference/fixtures/reference/object_callstmt_receiver.asp" \
        --root "$reference/fixtures/reference" | tr -d '\r\n')
    test "$receiver_output" = 'receiver:direct|receiver:computed'

    array_output=$("$program" \
        "$reference/fixtures/reference/fast_scalar_arrays.asp" \
        --root "$reference/fixtures/reference" | tr -d '\r\n')
    test "$array_output" = '6:15:9'

    isolation_output=$("$program" \
        "$reference/fixtures/reference/array_assignment_isolation.asp" \
        --root "$reference/fixtures/reference" | tr -d '\r\n')
    test "$isolation_output" = 'zero:changed'

    empty_array_output=$("$program" \
        "$reference/fixtures/reference/redim_empty_array.asp" \
        --root "$reference/fixtures/reference" | tr -d '\r\n')
    test "$empty_array_output" = 'top=;function=ready'

    frame_output=$("$program" \
        "$reference/fixtures/reference/tmeta_unified_frame.asp" \
        --root "$reference/fixtures/reference" | tr -d '\r\n')
    test "$frame_output" = \
        'added:alpha|alpha|one-stored|alpha=one-stored|rebound|rebound|1|kept,grown'
fi

// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`tests/test_core.c`): Native regression test. Local fixtures construct the
 * smallest state needed to exercise ownership, limits, persistence, and failure paths;
 * assertions document the externally observable contract and guard against silent regressions.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _POSIX_C_SOURCE 200809L

#include "ast_image.h"
#include "bytecode.h"
#include "lexer.h"
#include "openasp.h"
#include "parser.h"
#include "runtime.h"
#include "symbol.h"
#include "value.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_harness.h"

int main(void) {
    oasp_arena *arena = oasp_arena_create(64);
    assert(arena);
    assert(oasp_arena_alloc(arena, 7, 8));
    assert(oasp_arena_used(arena) == 7);
    assert(oasp_arena_reserved(arena) >= 4096);
    int finalized = 0;
    assert(oasp_arena_defer(arena, count_finalizer, &finalized));

    oasp_arena *borrowed_request_arena = oasp_arena_create(64);
    oasp_arena *borrowed_response_arena = oasp_arena_create(64);
    oasp_arena *borrowed_scratch_arena = oasp_arena_create(64);
    assert(borrowed_request_arena && borrowed_response_arena && borrowed_scratch_arena);
    char *borrowed_sentinel = oasp_arena_alloc(borrowed_request_arena, 16, _Alignof(char));
    assert(borrowed_sentinel);
    memcpy(borrowed_sentinel, "caller-owned", 13);
    int borrowed_finalized = 0;
    assert(oasp_arena_defer(borrowed_request_arena, count_finalizer, &borrowed_finalized));
    size_t borrowed_used = oasp_arena_used(borrowed_request_arena);
    oasp_request *borrowed_request =
        oasp_request_create_with_arenas(".", "", "", "GET", borrowed_request_arena,
                                        borrowed_response_arena, borrowed_scratch_arena);
    assert(borrowed_request);
    oasp_request_write(borrowed_request, "temporary", 9);
    oasp_request_destroy(borrowed_request);
    assert(!strcmp(borrowed_sentinel, "caller-owned"));
    assert(oasp_arena_used(borrowed_request_arena) == borrowed_used);
    assert(borrowed_finalized == 0);
    oasp_arena_destroy(borrowed_scratch_arena);
    oasp_arena_destroy(borrowed_response_arena);
    oasp_arena_destroy(borrowed_request_arena);
    assert(borrowed_finalized == 1);

    oasp_request *limited_request = oasp_request_create(".", "", "", "GET");
    assert(limited_request);
    oasp_request_limits memory_limits = {.memory_limit = 256 * 1024};
    assert(oasp_request_set_limits(limited_request, &memory_limits) == 0);
    assert(oasp_request_memory_usage(limited_request) > 0);
    assert(!oasp_arena_alloc(oasp_request_arena(limited_request), 256 * 1024, _Alignof(max_align_t)));
    assert(oasp_request_failed(limited_request));
    assert(strstr(oasp_request_error(limited_request), "allowed memory size of 262144 bytes exhausted"));
    assert(oasp_request_peak_memory_usage(limited_request) >= oasp_request_memory_usage(limited_request));
    oasp_request_destroy(limited_request);

    oasp_arena_reset(arena);
    assert(finalized == 1);
    oasp_arena_destroy(arena);

    oasp_arena *mapped_arena = oasp_arena_create_mapped(4096);
    assert(mapped_arena);
    oasp_arena_checkpoint checkpoint = oasp_arena_checkpoint_save(mapped_arena);
    assert(oasp_arena_alloc(mapped_arena, 8192, 16));
    int mapped_finalized = 0;
    assert(oasp_arena_defer(mapped_arena, count_finalizer, &mapped_finalized));
    assert(oasp_arena_reserved(mapped_arena) >= 8192);
    oasp_arena_checkpoint_restore(mapped_arena, checkpoint);
    assert(mapped_finalized == 1);
    assert(oasp_arena_used(mapped_arena) == 0);
    assert(oasp_arena_alloc(mapped_arena, 8192, 16));
    assert(oasp_arena_defer(mapped_arena, count_finalizer, &mapped_finalized));
    oasp_arena_reset(mapped_arena);
    assert(mapped_finalized == 2);
    assert(oasp_arena_used(mapped_arena) == 0);
    assert(oasp_arena_alloc(mapped_arena, 128, 16));
    oasp_arena_destroy(mapped_arena);

    oasp_request *request = oasp_request_create(".", "name=Open%20ASP&unicode=%u54C8%u54C8", "", "GET");
    assert(request);
    assert(oasp_arena_reserved(oasp_request_arena(request)) < 1024 * 1024);
    assert(!strcmp(oasp_request_query(request, "name"), "Open ASP"));
    assert(!strcmp(oasp_request_query(request, "NAME"), "Open ASP"));
    assert(!strcmp(oasp_request_query(request, "unicode"), "%u54C8%u54C8"));
    assert(!strcmp(oasp_request_query(request, "missing"), ""));
    const char *page = "<% x = \"hello\" & \" world\" %><%=x%>";
    assert(oasp_render_text(request, page, strlen(page), "test.asp") == 0);
    size_t length = 0;
    assert(!strcmp(oasp_request_output(request, &length), "hello world"));
    assert(length == strlen("hello world"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%=Server.HTMLEncode(\"<a&\"\"'>\")%>";
    assert(oasp_render_text(request, page, strlen(page), "htmlencode-entities.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "&lt;a&amp;&quot;&#39;&gt;"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Function BuildText()\n"
           "  Dim text, snapshot, i\n"
           "  text = \"\"\n"
           "  For i = 1 To 5\n"
           "    text = text & CStr(i)\n"
           "    If i = 2 Then snapshot = text\n"
           "  Next\n"
           "  BuildText = snapshot & \"|\" & text\n"
           "End Function\n"
           "Response.Write BuildText()\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "cow-string-builder.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "12|12345"));
    assert(oasp_arena_reserved(oasp_request_arena(request)) < 1024 * 1024);
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Sub AppendParts(ByRef output)\n"
           "  Dim i\n"
           "  output = \"\"\n"
           "  For i = 1 To 3\n"
           "    output = output & \"[\" & CStr(i) & \"]\"\n"
           "  Next\n"
           "End Sub\n"
           "Function FrameValue(ByVal seed)\n"
           "  Dim local\n"
           "  local = seed\n"
           "  FrameValue = local\n"
           "End Function\n"
           "Dim result\n"
           "Call AppendParts(result)\n"
           "Response.Write result & \"|\" & FrameValue(\"ok\")\n"
           "On Error Resume Next\n"
           "ignored = 1 / 0\n"
           "Response.Write \"|\" & CStr(Err.Number <> 0)\n"
           "On Error GoTo 0\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "optimized-vm-semantics.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "[1][2][3]|ok|True"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Function RecoverableFailure()\n"
           "  Dim missing\n"
           "  missing = Server.CreateObject(\"Legacy.Component\")\n"
           "  RecoverableFailure = \"continued\"\n"
           "End Function\n"
           "On Error Resume Next\n"
           "Response.Write RecoverableFailure()\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "caller-resume-next.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "continued"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Function ProbeUnavailableComponent()\n"
           "  On Error Resume Next\n"
           "  Dim object\n"
           "  Set object = Server.CreateObject(\"Legacy.Component\")\n"
           "  ProbeUnavailableComponent = (Err.Number = 429)\n"
           "  Err.Clear\n"
           "End Function\n"
           "Class AlertProbe\n"
           "  Public Sub Alert(message, target)\n"
           "    Response.Write message & \":\" & target\n"
           "  End Sub\n"
           "End Class\n"
           "Sub Submit(probe)\n"
           "  Dim title\n"
           "  title = \"\"\n"
           "  If title = \"\" Then\n"
           "    Call probe.Alert(\"请输入简短标题\", \"\")\n"
           "    Exit Sub\n"
           "  End If\n"
           "End Sub\n"
           "Dim probe\n"
           "Set probe = New AlertProbe\n"
           "Call Submit(probe)\n"
           "Response.Write \"|\" & ProbeUnavailableComponent()\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "call-member-resume-next.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "请输入简短标题:|True"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Function IsObjInstalled(ByVal strClassString)\n"
           "  Dim xTestObj, ClsString\n"
           "  On Error Resume Next\n"
           "  IsObjInstalled = False\n"
           "  ClsString = strClassString\n"
           "  Err = 0\n"
           "  Set xTestObj = Server.CreateObject(ClsString)\n"
           "  If Err = 0 Then IsObjInstalled = True\n"
           "  If Err = -2147352567 Then IsObjInstalled = True\n"
           "  Set xTestObj = Nothing\n"
           "  Err = 0\n"
           "  Exit Function\n"
           "End Function\n"
           "If Not IsObjInstalled(\"Gatherer.VBProcess\") Then Response.Write \"False\"\n"
           "Response.Write \"|after\"\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "resume-next-err-assignment.asp") == 0);
    const char *resume_next_output = oasp_request_output(request, &length);
    if (strcmp(resume_next_output, "False|after"))
        fprintf(stderr, "unexpected Err=0 resume-next output (%zu bytes): %s\n", length, resume_next_output);
    assert(!strcmp(resume_next_output, "False|after"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim inlineBranch\n"
           "If False Then inlineBranch = \"then\":Else:inlineBranch = \"else\"\n"
           "Response.Write inlineBranch\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "inline-if-else-separator.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "else"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    assert(oasp_request_set_cookie(request, "session", "safe") == 0);
    assert(oasp_request_set_cookie(request, "session", "unsafe\r\nX-Injected: yes") != 0);
    assert(oasp_request_set_cookie_attribute(request, "session", "path", "/safe") == 0);
    assert(oasp_request_set_cookie_attribute(request, "session", "path", "/\r\nX-Injected: yes") != 0);
    assert(strstr(oasp_request_header_value(request, 0), "\r") == NULL);
    assert(strstr(oasp_request_header_value(request, 0), "\n") == NULL);
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    test_response_sink sink = {0};
    oasp_request_set_output_sink(request, append_response, &sink);
    page = "<% Response.Buffer = False\n"
           "Response.Write \"A\"\n"
           "Response.Flush\n"
           "Response.Write \"B\"\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "streaming-response.asp") == 0);
    assert(sink.length == 2 && !memcmp(sink.data, "AB", 2));
    assert(sink.calls >= 3);
    assert(oasp_request_output_committed(request));
    assert(oasp_request_set_status(request, "201 Created") != 0);
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    char chunked[9000];
    memset(chunked, 'x', sizeof(chunked));
    oasp_request_write(request, chunked, sizeof(chunked));
    assert(oasp_request_output(request, &length));
    assert(length == sizeof(chunked));
    assert(!memcmp(oasp_request_output(request, &length), chunked, sizeof(chunked)));
    oasp_request_truncate_output(request, 4500);
    oasp_request_write(request, "tail", 4);
    const char *truncated = oasp_request_output(request, &length);
    assert(length == 4504);
    assert(!memcmp(truncated, chunked, 4500));
    assert(!memcmp(truncated + 4500, "tail", 4));
    memset(&sink, 0, sizeof(sink));
    oasp_request_set_output_sink(request, append_response, &sink);
    assert(oasp_request_flush(request) == 0);
    assert(sink.length == 4504 && sink.calls >= 2);
    assert(oasp_request_output(request, &length)[0] == '\0' && length == 0);
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<% Set d = Server.CreateObject(\"Scripting.Dictionary\")\n"
           "d.CompareMode = 1\n"
           "d.Add \"Alpha\", 1\n"
           "d.Add 42, 2\n"
           "For i = 1 To 100\n"
           "  If Not d.Exists(\"aLpHa\") Then Response.Write \"bad\"\n"
           "  If Not d.Exists(\"42\") Then Response.Write \"bad\"\n"
           "Next\n"
           "Response.Write d.Count\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "dictionary-compare.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "2"));
    assert(oasp_arena_reserved(oasp_request_arena(request)) < 1024 * 1024);
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<% Set d = Server.CreateObject(\"Scripting.Dictionary\")\n"
           "d.Add \"first\", 1\n"
           "d.Add 42, 2\n"
           "For Each key In d\n"
           "  Response.Write CStr(key) & \":\" & CStr(d(key)) & \"|\"\n"
           "Next\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "dictionary-enumeration.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "first:1|42:2|"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Sub Ping()\n"
           "  Response.Write \"sub|\"\n"
           "End Sub\n"
           "Class Probe\n"
           "  Public Sub Touch()\n"
           "    Response.Write \"method|\"\n"
           "  End Sub\n"
           "End Class\n"
           "Function Value()\n"
           "  Value = \"function\"\n"
           "End Function\n"
           "Dim item\n"
           "Ping\n"
           "Set item = New Probe\n"
           "item.Touch\n"
           "Response.Write Value\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "zero_arg_calls.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "sub|method|function"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Class Article\n"
           "  Public Property Get Url\n"
           "    Url = \"post\"\n"
           "  End Property\n"
           "  Public Property Get HtmlUrl\n"
           "    HtmlUrl = Url\n"
           "  End Property\n"
           "End Class\n"
           "Class ArticleList\n"
           "  Public Url\n"
           "  Public Function Render(item)\n"
           "    Render = item.HtmlUrl\n"
           "  End Function\n"
           "End Class\n"
           "Dim article, list\n"
           "Set article = New Article\n"
           "Set list = New ArticleList\n"
           "list.Url = \"default\"\n"
           "Response.Write list.Render(article)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "class-member-scope-isolation.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "post"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Function DynamicCheck(value)\n"
           "  DynamicCheck = (value = \"ok\")\n"
           "End Function\n"
           "Function ExecuteInFrame(value)\n"
           "  ExecuteInFrame = False\n"
           "  Execute \"ExecuteInFrame = DynamicCheck(value)\"\n"
           "End Function\n"
           "Response.Write CStr(ExecuteInFrame(\"ok\"))\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "execute_function_result.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "True"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Rem the lexer must ignore this entire line\n"
           "Const First = 1, Second = 2\n"
           "Class IndexedProbe\n"
           "  Private stored\n"
           "  Public Property Let Item(index, value)\n"
           "    stored = CStr(index) & \":\" & value\n"
           "  End Property\n"
           "  Public Property Get Text()\n"
           "    Text = stored\n"
           "  End Property\n"
           "End Class\n"
           "Dim [dictionary]\n"
           "Dim probe\n"
           "Set [dictionary] = Server.CreateObject(\"Scripting.Dictionary\")\n"
           "[dictionary].Add \"key\", \"value\"\n"
           "Set probe = New IndexedProbe\n"
           "probe.Item(7) = \"ok\"\n"
           "On Error Resume Next\n"
           "For number = \"invalid\" To 1\n"
           "Next\n"
           "For Each element In 42\n"
           "Next\n"
           "Response.Write [dictionary].Item(\"key\") & \"|\"\n"
           "Response.Write CStr(First + Second) & \"|\" & probe.Text & \"|\"\n"
           "Response.Write 999999999999999999999999999999999999999999\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "bnf_regressions.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "value|3|7:ok|-6640025486929952769"));
    oasp_request_destroy(request);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "On Error Resume Next\n"
           "Err.Raise 123\n"
           "Response.Write CStr(Err) & \"|\" & IsDate(Date)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "bare-err-date.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "123|True"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<% Response.Write Year(CDate(\"08/13/09 11:41:52\")) & \":\" & "
           "Month(CDate(\"08/13/09 11:41:52\")) & \":\" & "
           "Second(CDate(\"08/13/09 11:41:52\")) %>";
    assert(oasp_render_text(request, page, strlen(page), "cdate-jet-datetime.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "2009:8:52"));
    oasp_request_destroy(request);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Select Case 2\n"
           "Case 1 Response.Write \"wrong\"\n"
           "Case 2 Call Response.Write(\"inline\")\n"
           "Case Else Response.Write \"wrong\"\n"
           "End Select\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "inline-case.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "inline"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim authenticated\n"
           "authenticated = True\n"
           "Select Case \"home\"\n"
           "Case \"home\"\n"
           "  If authenticated Then\n"
           "    Response.Write \"authenticated\"\n"
           "  Else\n"
           "    Response.Write \"guest\"\n"
           "  End If\n"
           "  Response.Write \"|feed\"\n"
           "End Select\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "select-nested-if.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "authenticated|feed"));
    oasp_request_destroy(request);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Function InlineExit(value)\n"
           "  InlineExit = \"continued\"\n"
           "  If value = 0 Then InlineExit = \"stopped\":Exit Function\n"
           "  InlineExit = \"ok\"\n"
           "End Function\n"
           "Function InlineElse(value)\n"
           "  If value Then InlineElse = \"A\":InlineElse = InlineElse & \"B\" "
           "Else InlineElse = \"C\":InlineElse = InlineElse & \"D\"\n"
           "End Function\n"
           "Response.Write InlineExit(1) & \"|\" & InlineExit(0) & \"|\" & "
           "InlineElse(True) & \"|\" & InlineElse(False)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "inline-if-statements.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "ok|stopped|AB|CD"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Option Explicit\n"
           "Function NestedDim(enabled)\n"
           "  If enabled Then\n"
           "    Dim branchLocal\n"
           "    branchLocal = \"set\"\n"
           "  End If\n"
           "  NestedDim = IsEmpty(branchLocal)\n"
           "End Function\n"
           "Response.Write NestedDim(False) & \"|\" & NestedDim(True)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "nested-dim-scope.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "True|False"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Class Item\n"
           "  Public Name\n"
           "End Class\n"
           "Class Box\n"
           "  Public ItemIndex\n"
           "  Public Property Get Value\n"
           "    Value = Items(ItemIndex).Name\n"
           "  End Property\n"
           "End Class\n"
           "Dim Items(0), Entry\n"
           "Set Items(0) = New Item\n"
           "Items(0).Name = \"ok\"\n"
           "Function ReadBox(OuterIndex)\n"
           "  Dim ValueBox\n"
           "  Set ValueBox = New Box\n"
           "  ValueBox.ItemIndex = 0\n"
           "  ReadBox = ValueBox.Value\n"
           "End Function\n"
           "Response.Write ReadBox(5)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "nested-prepared-member.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "ok"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Class SourceBox\n"
           "  Public Function GetValue(Name)\n"
           "    GetValue = \"inner\"\n"
           "  End Function\n"
           "End Class\n"
           "Class TargetBox\n"
           "  Public Stored\n"
           "  Public Property Let Value(Input)\n"
           "    Stored = Input\n"
           "  End Property\n"
           "End Class\n"
           "Class ConfigBox\n"
           "  Public Target\n"
           "  Public Function Load(Name)\n"
           "    Target.Value = Source.GetValue(Name)\n"
           "    Load = Target.Stored\n"
           "  End Function\n"
           "End Class\n"
           "Dim Source, Target, Config\n"
           "Set Source = New SourceBox\n"
           "Set Target = New TargetBox\n"
           "Set Config = New ConfigBox\n"
           "Set Config.Target = Target\n"
           "Response.Write Config.Load(\"outer\")\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "nested-prepared-property.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "inner"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Option Explicit\n"
           "If False Then Dim pageLocal\n"
           "Randomize 123\n"
           "Dim sample\n"
           "sample = Rnd\n"
           "Response.Write IsEmpty(pageLocal) & \"|\" & "
           "(sample >= 0) & \"|\" & (sample < 1) & \"|\" & "
           "Int(-1.2) & \"|\" & Fix(-1.2) & \"|\" & "
           "InStr(1, \"AbC\", \"bc\", vbTextCompare)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "global-dim-random.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "True|True|True|-2|-1|2"));
    oasp_request_destroy(request);
    request = oasp_request_create("tests/fixtures/common", "", "", "GET");
    assert(request);
    oasp_gc_config gc_config = {.heap_limit = 8 * 1024 * 1024,
                                .young_limit = 16 * 1024,
                                .major_threshold = 32 * 1024};
    oasp_gc_stats gc_stats;
    assert(oasp_render_file_service(request, "tests/fixtures/common/service_format_gc.asp",
                                    &gc_config, &gc_stats) == 0);
    const char *format_output = oasp_request_output(request, &length);
    assert(length == 7 && !memcmp(format_output, "100000\n", 7));
    assert(oasp_arena_used(oasp_request_arena(request)) < 512 * 1024);
    assert(gc_stats.allocation_failures == 0);
    oasp_request_destroy(request);

    request = oasp_request_create("tests/fixtures/common", "", "", "GET");
    assert(request);
    memset(&gc_stats, 0, sizeof(gc_stats));
    assert(oasp_render_file_service(request, "tests/fixtures/common/service_json_format_gc.asp",
                                    &gc_config, &gc_stats) == 0);
    const char *json_format_output = oasp_request_output(request, &length);
    assert(length == 7 && !memcmp(json_format_output, "100000\n", 7));
    assert(oasp_arena_used(oasp_request_arena(request)) < 512 * 1024);
    assert(gc_stats.allocation_failures == 0);
    oasp_request_destroy(request);

    request = oasp_request_create("tests/fixtures/common", "iterations=4000", "", "GET");
    assert(request);
    memset(&gc_stats, 0, sizeof(gc_stats));
    assert(oasp_render_file_service(request, "tests/fixtures/common/runtime_long_stress.asp",
                                    &gc_config, &gc_stats) == 0);
    const char *stress_output = oasp_request_output(request, &length);
    if (strcmp(stress_output, "stress-ok|4000|456719|4000|127\n"))
        fprintf(stderr, "unexpected runtime stress output (%zu bytes): %s\n", length, stress_output);
    assert(!strcmp(stress_output, "stress-ok|4000|456719|4000|127\n"));
    assert(oasp_arena_peak_used(oasp_request_scratch_arena(request)) < 64 * 1024);
    assert(gc_stats.allocation_failures == 0);
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Class ArticleProbe\n"
           "  Public Function LoadInfobyID(ByRef identifier)\n"
           "    LoadInfobyID = True\n"
           "    Call Filter(identifier)\n"
           "  End Function\n"
           "End Class\n"
           "Function Filter(ByRef identifier)\n"
           "End Function\n"
           "Dim article, identifier\n"
           "identifier = 1\n"
           "Set article = New ArticleProbe\n"
           "Response.Write article.LoadInfoByID(identifier)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "class-function-result-byref.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "True"));
    oasp_request_destroy(request);
    return 0;
}

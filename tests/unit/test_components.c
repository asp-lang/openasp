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
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_harness.h"

int main(void) {
    oasp_request *request;
    const char *page;
    const char *binary_output;
    size_t length = 0;
    char original_directory[PATH_MAX];
    char sqlite_directory[PATH_MAX];
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim doc, declaration, root\n"
           "Set doc = Server.CreateObject(\"Microsoft.XMLDOM\")\n"
           "Set declaration = doc.createProcessingInstruction("
           "\"xml\", \"version=\"\"1.0\"\" encoding=\"\"UTF-8\"\"\")\n"
           "doc.insertBefore declaration, doc.childNodes(0)\n"
           "Set root = doc.createElement(\"rss\")\n"
           "root.setAttribute \"version\", \"2.0\"\n"
           "doc.appendChild root\n"
           "Response.Write doc.xml\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "xml.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "<?xml version=\"1.0\" encoding=\"UTF-8\"?><rss "
                                                          "version=\"2.0\"/>"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim doc, root, description, cdata\n"
           "Set doc = Server.CreateObject(\"Microsoft.XMLDOM\")\n"
           "Set root = doc.createElement(\"item\")\n"
           "Set description = doc.createElement(\"description\")\n"
           "Call root.appendChild(description)\n"
           "Set cdata = doc.createNode(\"cdatasection\", \"\", \"\")\n"
           "cdata.nodeValue = \"alpha ]]> beta\"\n"
           "Call description.appendChild(cdata)\n"
           "Call doc.appendChild(root)\n"
           "Response.Write doc.xml\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "xml-cdata-string-node-type.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length),
                   "<item><description><![CDATA[alpha ]]]]><![CDATA[> beta]]></description></item>"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim doc, root\n"
           "Set doc = Server.CreateObject(\"MSXML2.FreeThreadedDOMDocument.3.0\")\n"
           "Set root = doc.createElement(\"root\")\n"
           "doc.appendChild root\n"
           "Response.Write doc.xml\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "freethreaded-domdocument.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "<root/>"));
    oasp_request_destroy(request);

    const char *xml_save_path = "build/tests/test-xml-save.xml";
    (void)remove(xml_save_path);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim doc, declaration, root\n"
           "Set doc = Server.CreateObject(\"Microsoft.XMLDOM\")\n"
           "Set declaration = doc.createProcessingInstruction("
           "\"xml\", \"version=\"\"1.0\"\" encoding=\"\"UTF-8\"\"\")\n"
           "doc.insertBefore declaration, doc.childNodes(0)\n"
           "Set root = doc.createElement(\"rss\")\n"
           "doc.appendChild root\n"
           "doc.save \"build/tests/test-xml-save.xml\"\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "xml-save.asp") == 0);
    FILE *xml_file = fopen(xml_save_path, "rb");
    assert(xml_file);
    char xml_saved[256];
    size_t xml_saved_length = fread(xml_saved, 1, sizeof(xml_saved) - 1, xml_file);
    assert(!ferror(xml_file));
    assert(!fclose(xml_file));
    xml_saved[xml_saved_length] = '\0';
    const char *xml_declaration = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    assert(!strncmp(xml_saved, xml_declaration, strlen(xml_declaration)));
    assert(!strstr(xml_saved + strlen(xml_declaration), "<?xml"));
    assert(strstr(xml_saved, "<rss/>"));
    assert(!remove(xml_save_path));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "\r\n\r\n"
           "<% Response.ContentType = \"text/xml\" %>\r\n"
           "<% Response.Write \"<?xml version=\"\"1.0\"\"?><root/>\" %>";
    assert(oasp_render_text(request, page, strlen(page), "xml-leading-whitespace.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "<?xml version=\"1.0\"?><root/>"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim doc\n"
           "Set doc = Server.CreateObject(\"Microsoft.XMLDOM\")\n"
           "doc.async = False\n"
           "doc.validateOnParse = False\n"
           "Call doc.loadXML(\"<root/>\")\n"
           "Response.Write doc.readyState & \":\" & doc.parseError.errorCode\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "xml-state.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "4:0"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim doc, oldRoot, ignored\n"
           "Set doc = Server.CreateObject(\"Microsoft.XMLDOM\")\n"
           "Call doc.loadXML(\"<old/>\")\n"
           "Set oldRoot = doc.documentElement\n"
           "Call doc.loadXML(\"<new/>\")\n"
           "On Error Resume Next\n"
           "ignored = oldRoot.nodeName\n"
           "Response.Write (Err.Number <> 0)\n"
           "Err.Clear\n"
           "Response.Write \":\" & doc.documentElement.nodeName\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "xml-reload-invalidates-nodes.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "True:new"));
    oasp_request_destroy(request);

    assert(setenv("OPENASP_PROCESS_ENABLE", "on", 1) == 0);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim process, input\n"
           "Set process = Server.CreateObject(\"OpenASP.Process\")\n"
           "input = String(131072, \"x\")\n"
           "Call process.Run(\"/bin/sh\", \"-c 'yes y | head -c 131072; cat >/dev/null; printf done'\", \"\", input, 5000)\n"
           "Response.Write Len(process.StdOut) & \":\" & process.ExitCode\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "process-duplex-pipes.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "131076:0"));
    oasp_request_destroy(request);
    assert(unsetenv("OPENASP_PROCESS_ENABLE") == 0);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Function IsSelf(value)\n"
           "IsSelf = IsObject(value)\n"
           "End Function\n"
           "Class Probe\n"
           "Public Sub Verify()\n"
           "Response.Write IsSelf(Me)\n"
           "End Sub\n"
           "End Class\n"
           "Dim probe\n"
           "Set probe = New Probe\n"
           "Call probe.Verify()\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "me.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "True"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim recordset, rows\n"
           "Set recordset = Server.CreateObject(\"ADODB.Recordset\")\n"
           "rows = recordset.GetRows\n"
           "Response.Write IsArray(rows) & \":\" & UBound(rows, 1) & "
           "\":\" & UBound(rows, 2)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "getrows.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "True:-1:-1"));
    oasp_request_destroy(request);

    assert(getcwd(original_directory, sizeof(original_directory)));
    assert(snprintf(sqlite_directory, sizeof(sqlite_directory), "/tmp/openasp-sqlite-memory-%ld", (long)getpid()) > 0);
    assert(mkdir(sqlite_directory, 0700) == 0);
    assert(chdir(sqlite_directory) == 0);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim connection, recordset, field, names\n"
           "Set connection = Server.CreateObject(\"ADODB.Connection\")\n"
           "connection.Open \"Provider=SQLite;Data Source=:memory:\"\n"
           "connection.Execute \"CREATE TABLE entries (title TEXT, rank INTEGER)\"\n"
           "connection.Execute \"INSERT INTO entries VALUES ('first', 7)\"\n"
           "Set recordset = connection.Execute(\"SELECT title, rank FROM entries\")\n"
           "For Each field In recordset.Fields\n"
           "  names = names & field.Name & \":\" & field.Value & \"|\"\n"
           "Next\n"
           "Response.Write recordset.Fields.Count & \";\" & names\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "ado-fields-enumeration.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "2;title:first|rank:7|"));
    oasp_request_destroy(request);
    assert(access(":memory:", F_OK) != 0);
    assert(chdir(original_directory) == 0);
    assert(rmdir(sqlite_directory) == 0);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim connection, rows, emptyRows\n"
           "Set connection = Server.CreateObject(\"ADODB.Connection\")\n"
           "connection.Open \"Provider=SQLite;Data Source=:memory:\"\n"
           "Set rows = connection.Execute(\"SELECT 'value' AS value\")\n"
           "Set emptyRows = connection.Execute(\"SELECT 'value' AS value WHERE 1=0\")\n"
           "Response.Write CStr(rows.BOF) & \":\" & CStr(rows.EOF) & \":\" & "
           "CStr(emptyRows.BOF) & \":\" & CStr(emptyRows.EOF)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "ado-recordset-bof.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "False:False:True:True"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim connection, recordset\n"
           "Set connection = Server.CreateObject(\"ADODB.Connection\")\n"
           "connection.Open \"Provider=SQLite;Data Source=:memory:\"\n"
           "Set recordset = connection.Execute(\"SELECT 'one' AS value UNION ALL SELECT 'two' UNION ALL SELECT 'three'\")\n"
           "recordset.PageSize = 2\n"
           "Response.Write recordset.PageSize & \":\" & recordset.PageCount & \":\"\n"
           "recordset.AbsolutePage = 2\n"
           "Response.Write recordset.AbsolutePage & \":\" & recordset(0)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "ado-recordset-pagination.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "2:2:2:three"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim connection, recordset\n"
           "Set connection = Server.CreateObject(\"ADODB.Connection\")\n"
           "connection.Open \"Provider=SQLite;Data Source=:memory:\"\n"
           "Set recordset = Server.CreateObject(\"ADODB.Recordset\")\n"
           "Set recordset.ActiveConnection = connection\n"
           "recordset.Open \"SELECT 'value' AS title\"\n"
           "Response.Write IsObject(recordset.ActiveConnection) & \":\"\n"
           "Set recordset.ActiveConnection = Nothing\n"
           "Response.Write IsObject(recordset.ActiveConnection)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "ado-recordset-active-connection.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "True:False"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim connection, recordset, pass\n"
           "Set connection = Server.CreateObject(\"ADODB.Connection\")\n"
           "connection.Open \"Provider=SQLite;Data Source=:memory:\"\n"
           "Set recordset = Server.CreateObject(\"ADODB.Recordset\")\n"
           "For pass = 1 To 2\n"
           "  If pass = 1 Then\n"
           "    recordset.Open \"SELECT 'first' AS title, 7 AS rank\", connection\n"
           "  Else\n"
           "    recordset.Open \"SELECT 9 AS rank, 'second' AS title\", connection\n"
           "  End If\n"
           "  Response.Write recordset.Fields(\"title\")\n"
           "  recordset.Close\n"
           "Next\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "recordset-schema-cache.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "firstsecond"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim connection, recordset, saved\n"
           "Set connection = Server.CreateObject(\"ADODB.Connection\")\n"
           "connection.Open \"Provider=SQLite;Data Source=:memory:\"\n"
           "Set recordset = connection.Execute(\"SELECT 'first' AS title UNION ALL SELECT 'second' AS title\")\n"
           "saved = CStr(recordset.Fields(\"title\"))\n"
           "recordset.MoveNext\n"
           "recordset.Close\n"
           "recordset.Open \"SELECT 'replacement' AS title\", connection\n"
           "Response.Write saved & \":\" & recordset.Fields(\"title\")\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "recordset-field-lifetime.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "first:replacement"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim connection, recordset, rows, saved\n"
           "Set connection = Server.CreateObject(\"ADODB.Connection\")\n"
           "connection.Open \"Provider=SQLite;Data Source=:memory:\"\n"
           "Set recordset = connection.Execute(\"SELECT 'first' AS title UNION ALL SELECT 'second' UNION ALL SELECT 'third' UNION ALL SELECT 'fourth'\")\n"
           "saved = recordset(0)\n"
           "recordset.MoveNext\n"
           "rows = recordset.GetRows(1)\n"
           "Response.Write saved & \":\" & rows(0, 0) & \":\" & recordset(0) & \":\" & CStr(recordset.EOF) & \":\"\n"
           "recordset.MoveNext\n"
           "Response.Write recordset(0) & \":\"\n"
           "recordset.MoveNext\n"
           "Response.Write CStr(recordset.EOF)\n"
           "Set recordset = connection.Execute(\"SELECT 'unread' AS title UNION ALL SELECT 'closed'\")\n"
           "connection.Close\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "sqlite-recordset-stream.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "first:second:third:False:fourth:True"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim connection, outerRows, innerRows, output\n"
           "Set connection = Server.CreateObject(\"ADODB.Connection\")\n"
           "connection.Open \"Provider=SQLite;Data Source=:memory:\"\n"
           "Set outerRows = connection.Execute(\"SELECT 'one' AS value UNION ALL SELECT 'two' UNION ALL SELECT 'three'\")\n"
           "Do While Not outerRows.EOF\n"
           "  Set innerRows = connection.Execute(\"SELECT 'tag' AS value\")\n"
           "  output = output & outerRows(0) & innerRows(0)\n"
           "  innerRows.Close\n"
           "  outerRows.MoveNext\n"
           "Loop\n"
           "connection.Close\n"
           "Response.Write output\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "sqlite-nested-recordsets.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "onetagtwotagthreetag"));
    oasp_request_destroy(request);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim stream\n"
           "Set stream = Server.CreateObject(\"ADODB.Stream\")\n"
           "stream.Open\n"
           "stream.WriteText \"stale\"\n"
           "stream.Close\n"
           "stream.Open\n"
           "Response.Write stream.Size & \"|\"\n"
           "stream.WriteText = \"fresh\"\n"
           "stream.Position = 0\n"
           "Response.Write stream.ReadText\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "stream-reopen.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "0|fresh"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%= Escape(\"A b/中\") & \"|\" & "
           "Unescape(\"A%20b/%u4E2D\") & \"|\" & "
           "vbsescape(\"{%date%}/中\") & \"|\" & "
           "vbsunescape(vbsescape(\"{%date%}/中\")) %>";
    assert(oasp_render_text(request, page, strlen(page), "escape.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "A%20b/%u4E2D|A b/中|%7B%25date%25%7D/%u4E2D|{%date%}/中"));
    oasp_request_destroy(request);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim payload\n"
           "payload = \"A\" & ChrB(0) & \"BC\"\n"
           "Response.Write Len(payload) & \"|\" & LenB(payload) & \"|\"\n"
           "Response.Write AscB(MidB(payload, 2, 1)) & \"|\"\n"
           "Response.Write LenB(MidB(payload, 2, 2)) & \"|\" & CDbl(\"12.5\")\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "binary-builtins-cdbl.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "4|4|0|2|12.5"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim payload\n"
           "payload = \"header\" & ChrB(0) & \"image\" & vbCrLf & \"--boundary\"\n"
           "Response.Write InStrB(1, payload, vbCrLf & \"--boundary\") & \"|\"\n"
           "Response.Write InStrB(8, payload, vbCrLf & \"--boundary\")\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "instrb-binary-boundary.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "13|13"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim expression, matches, match\n"
           "Set expression = New RegExp\n"
           "expression.Pattern = \"(ab)\"\n"
           "expression.Global = True\n"
           "Set matches = expression.Execute(\"zab\")\n"
           "Set match = matches.Item(0)\n"
           "Response.Write match.Value & \"|\" & match.SubMatches.Item(0) & \"|\" & match.Length\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "regexp-collection-item.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "ab|ab|2"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim expression\n"
           "Set expression = New RegExp\n"
           "expression.MultiLine = True\n"
           "expression.Pattern = \"^second$\"\n"
           "Response.Write expression.Test(\"first\" & vbCrLf & \"second\") & \"|\"\n"
           "Response.Write ScriptEngine & \":\" & ScriptEngineMajorVersion & \".\" & ScriptEngineMinorVersion\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "regexp-multiline-script-engine.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "True|VBScript:5.8"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Class DefaultPair\n"
           "  Private values\n"
           "  Private Sub Class_Initialize\n"
           "    Set values = CreateObject(\"Scripting.Dictionary\")\n"
           "  End Sub\n"
           "  Public Property Let Pair(key, value)\n"
           "    values(key) = value\n"
           "  End Property\n"
           "  Public Property Set Pair(key, value)\n"
           "    Set values(key) = value\n"
           "  End Property\n"
           "  Public Default Property Get Pair(key)\n"
           "    If IsObject(values(key)) Then Set Pair = values(key) Else Pair = values(key)\n"
           "  End Property\n"
           "End Class\n"
           "Dim pair, nested\n"
           "Set pair = New DefaultPair\n"
           "pair(\"text\") = \"value\"\n"
           "Set nested = New DefaultPair\n"
           "Set pair(\"object\") = nested\n"
           "Response.Write pair(\"text\") & \"|\" & TypeName(pair(\"object\"))\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "default-property-pair-assign.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "value|Object"));
    oasp_request_destroy(request);

    request = oasp_request_create("tests/fixtures", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Dim fso, folder, file\n"
           "Set fso = Server.CreateObject(\"Scripting.FileSystemObject\")\n"
           "Set folder = fso.GetFolder(Server.MapPath(\".\"))\n"
           "Set file = fso.GetFile(Server.MapPath(\"reference/fso_date_last_modified.asp\"))\n"
           "Response.Write TypeName(folder.DateLastModified) & \"|\" & (file.Size > 0) & \"|\" & (folder.Size >= file.Size)\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "fso-folder-date-last-modified.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "Date|True|True"));
    oasp_request_destroy(request);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<script language=\"javascript\" runat=\"server\">\n"
           "function MapCharacters(value) {\n"
           "  var s=\"AB\";\n"
           "  var t='甲乙';\n"
           "  var output='';\n"
           "  for(var i=0;i<value.length;i++)\n"
           "    output+=(t.indexOf(value.charAt(i))==-1)"
           "?value.charAt(i):s.charAt(t.indexOf(value.charAt(i)));\n"
           "  return output;\n"
           "}\n"
           "</script>"
           "<script language=\"javascript\" runat=\"server\">\n"
           "function Unsupported() { return \"not rendered\"; }\n"
           "</script>"
           "<%=MapCharacters(\"甲x乙\")%>|"
           "<%=Len(\"甲乙\")%>|<%=Mid(\"甲乙\", 2, 1)%>|"
           "<%=InStr(1, \"a甲乙\", \"乙\")%>";
    assert(oasp_render_text(request, page, strlen(page), "server-js-character-map.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "AxB|2|乙|3"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<script language=\"vbscript\" runat=\"server\">\n"
           "Function CaptchaPrefix()\n"
           "  CaptchaPrefix = ChrB(Asc(\"G\")) & ChrB(Asc(\"I\")) & ChrB(Asc(\"F\"))\n"
           "End Function\n"
           "Response.ContentType = \"image/gif\"\n"
           "Response.BinaryWrite CaptchaPrefix()\n"
           "</script>";
    assert(oasp_render_text(request, page, strlen(page), "server-vbscript-binary.asp") == 0);
    binary_output = oasp_request_output(request, &length);
    assert(length == 3);
    assert(!memcmp(binary_output, "GIF", 3));
    oasp_request_destroy(request);
    return 0;
}

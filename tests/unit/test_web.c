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
#include <unistd.h>

#include "test_harness.h"

int main(void) {
    oasp_request *request;
    const char *page;
    size_t length = 0;
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Response.Status = \"201 Created\"\n"
           "Response.ContentType = \"application/json\"\n"
           "Response.AddHeader \"X-OpenASP\", \"runtime\"\n"
           "Response.Cookies(\"session\") = \"abc\"\n"
           "Response.Cookies(\"session\").Path = \"/app\"\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "response.asp") == 0);
    assert(!strcmp(oasp_request_status(request), "201 Created"));
    assert(!strcmp(oasp_request_content_type(request), "application/json"));
    assert(oasp_request_header_count(request) == 2);
    assert(!strcmp(oasp_request_header_name(request, 0), "X-OpenASP"));
    assert(!strcmp(oasp_request_header_value(request, 0), "runtime"));
    assert(!strcmp(oasp_request_header_name(request, 1), "Set-Cookie"));
    assert(!strcmp(oasp_request_header_value(request, 1), "session=abc; Path=/app"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Response.ContentType = \"application/json; profile=legacy; charset=us-ascii\"\n"
           "Response.Charset = \"gb2312\"\n"
           "Response.Write Response.Charset\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "response-charset.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "gb2312"));
    assert(!strcmp(oasp_request_content_type(request), "application/json; profile=legacy; charset=gb2312"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Response.ContentType = \"text/plain\"\n"
           "Response.CodePage = 65001\n"
           "Response.Write Response.Charset\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "response-codepage.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "utf-8"));
    assert(!strcmp(oasp_request_content_type(request), "text/plain; charset=utf-8"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Response.CodePage = 936\n"
           "Response.Write \"\xE6\x8A\x80\xE6\x9C\xAF\xE7\xA7\x91\"\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "response-gbk.asp") == 0);
    assert(!memcmp(oasp_request_output(request, &length), "\xBC\xBC\xCA\xF5\xBF\xC6", 6));
    assert(length == 6);
    assert(!strcmp(oasp_request_content_type(request), "text/html; charset=gbk"));
    oasp_request_destroy(request);

    char codepage_path[] = "/tmp/openasp-codepage-XXXXXX";
    int codepage_fd = mkstemp(codepage_path);
    assert(codepage_fd >= 0);
    FILE *codepage_file = fdopen(codepage_fd, "wb");
    assert(codepage_file);
    const char codepage_source[] = "<%@ CODEPAGE=936 %><% Response.Write \"\xBC\xBC\xCA\xF5\xBF\xC6\" %>";
    assert(fwrite(codepage_source, 1, sizeof(codepage_source) - 1, codepage_file) == sizeof(codepage_source) - 1);
    assert(!fclose(codepage_file));
    request = oasp_request_create("/tmp", "", "", "GET");
    assert(request);
    assert(oasp_render_file(request, codepage_path) == 0);
    assert(!memcmp(oasp_request_output(request, &length), "\xBC\xBC\xCA\xF5\xBF\xC6", 6));
    assert(length == 6);
    oasp_request_destroy(request);

    codepage_file = fopen(codepage_path, "wb");
    assert(codepage_file);
    const char inferred_source[] = "<% Response.Write \"\xBC\xBC\xCA\xF5\xBF\xC6\" %>";
    assert(fwrite(inferred_source, 1, sizeof(inferred_source) - 1, codepage_file) == sizeof(inferred_source) - 1);
    assert(!fclose(codepage_file));
    request = oasp_request_create("/tmp", "", "", "GET");
    assert(request);
    assert(oasp_render_file(request, codepage_path) == 0);
    assert(!memcmp(oasp_request_output(request, &length), "\xBC\xBC\xCA\xF5\xBF\xC6", 6));
    assert(length == 6);
    assert(!strcmp(oasp_request_content_type(request), "text/html; charset=gbk"));
    oasp_request_destroy(request);
    assert(!unlink(codepage_path));

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Response.Write \"before\"\n"
           "Response.Flush\n"
           "Session.Abandon\n"
           "Response.Write \"after\"\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "zero-argument-members.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "beforeafter"));
    oasp_request_destroy(request);

    assert(setenv("HTTP_COOKIE", "username=admin; password=abc123; spaced=value", 1) == 0);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<% Response.Write Request.Cookies(\"username\") & \":\" & "
           "Request.Cookies(\"password\") & \":\" & "
           "Request.Cookies(\"spaced\") & \":\" & "
           "Request.Cookies(\"missing\") %>";
    assert(oasp_render_text(request, page, strlen(page), "cookies.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "admin:abc123:value:"));
    oasp_request_destroy(request);
    assert(unsetenv("HTTP_COOKIE") == 0);

    assert(setenv("HTTP_COOKIE", "ACTCMS=UserName=member&PassWord=secret&GroupID=3", 1) == 0);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<% Response.Write Request.Cookies(\"ACTCMS\")(\"UserName\") & \":\" & "
           "Request.Cookies(\"ACTCMS\")(\"PassWord\") & \":\" & "
           "Request.Cookies(\"ACTCMS\")(\"missing\") %>";
    assert(oasp_render_text(request, page, strlen(page), "cookie-subvalues.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "member:secret:"));
    oasp_request_destroy(request);
    assert(unsetenv("HTTP_COOKIE") == 0);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "Response.Cookies(\"ACTCMS\")(\"UserName\") = \"member\"\n"
           "Response.Cookies(\"ACTCMS\")(\"PassWord\") = \"secret\"\n"
           "Response.Cookies(\"ACTCMS\")(\"UserName\") = \"updated\"\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "cookie-subvalue-response.asp") == 0);
    assert(oasp_request_header_count(request) == 1);
    assert(!strcmp(oasp_request_header_name(request, 0), "Set-Cookie"));
    assert(!strcmp(oasp_request_header_value(request, 0), "ACTCMS=UserName=updated&PassWord=secret"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    assert(oasp_request_set_server_variable(request, "SERVER_NAME", "request.example") == 0);
    assert(oasp_request_set_server_variable(request, "HTTP_COOKIE", "username=request-user") == 0);
    page = "<% Response.Write Request.ServerVariables(\"server_name\") & \":\" & Request.Cookies(\"username\") %>";
    assert(oasp_render_text(request, page, strlen(page), "server-variables.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "request.example:request-user"));
    oasp_request_destroy(request);
    request = oasp_request_create("/tmp/openasp-root", "", "", "GET");
    assert(request);
    page = "<%= Server.MapPath(\"child\\file.txt\") & \"|\" & Server.MapPath(\"/global.txt\") %>";
    assert(oasp_render_text(request, page, strlen(page), "map-path.asp") == 0);
#if defined(_WIN32)
    assert(!strcmp(oasp_request_output(request, &length), "/tmp/openasp-root\\child\\file.txt|/tmp/openasp-root\\global.txt"));
#else
    assert(!strcmp(oasp_request_output(request, &length), "/tmp/openasp-root/child/file.txt|/tmp/openasp-root/global.txt"));
#endif
    oasp_request_destroy(request);

    request = oasp_request_create(".", "present=&query=value", "form=posted", "POST");
    assert(request);
    page = "<%= IsEmpty(Request.QueryString(\"missing\")) & \"|\" & "
           "IsEmpty(Request.QueryString(\"present\")) & \"|\" & "
           "Request(\"QUERY\") & \"|\" & Request.Form(\"FORM\") & \"|\" & "
           "Request.QueryString.Count & \"|\" & Request.Form.Count %>";
    assert(oasp_render_text(request, page, strlen(page), "request-empty.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "True|False|value|posted|2|1"));
    oasp_request_destroy(request);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "\xEF\xBB\xBF\r\n \t<%\n"
           "Response.ContentType = \"image/gif\"\n"
           "Response.BinaryWrite \"GIF89a\"\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "binary-whitespace-prefix.asp") == 0);
    const char *binary_output = oasp_request_output(request, &length);
    assert(length == 6);
    assert(!memcmp(binary_output, "GIF89a", 6));
    oasp_request_destroy(request);
    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "visible<%\n"
           "Response.ContentType = \"application/octet-stream\"\n"
           "Response.BinaryWrite \"data\"\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "binary-visible-prefix.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "visibledata"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "before<% = \"whitespace-expression\" %>after";
    assert(oasp_render_text(request, page, strlen(page), "whitespace-expression.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "beforewhitespace-expressionafter"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "With Response\n"
           "  .Write \"with-response-member\"\n"
           "End With\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "with-response-member.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "with-response-member"));
    oasp_request_destroy(request);

    request = oasp_request_create(".", "", "", "GET");
    assert(request);
    page = "<%\n"
           "With Response\n"
           "  Response.Write \"with-response\"\n"
           "End With\n"
           "%>";
    assert(oasp_render_text(request, page, strlen(page), "with-response.asp") == 0);
    assert(!strcmp(oasp_request_output(request, &length), "with-response"));
    oasp_request_destroy(request);
    return 0;
}

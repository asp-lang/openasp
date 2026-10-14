// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "component.h"

static const oasp_component_registration registry[] = {
    {"scripting.dictionary", OASP_COMPONENT_DICTIONARY, "Scripting.Dictionary"},
    {"scripting.filesystemobject", OASP_COMPONENT_FSO, "Scripting.FileSystemObject"},
    {"vbscript.regexp", OASP_COMPONENT_REGEXP, "VBScript.RegExp"},
    {"regexp", OASP_COMPONENT_REGEXP, "VBScript.RegExp"},
    {"adodb.stream", OASP_COMPONENT_STREAM, "ADODB.Stream"},
    {"microsoft.xmldom", OASP_COMPONENT_XML, "Microsoft.XMLDOM"},
    {"msxml2.domdocument", OASP_COMPONENT_XML, "MSXML2.DOMDocument"},
    {"msxml2.domdocument.3.0", OASP_COMPONENT_XML, "MSXML2.DOMDocument"},
    {"msxml2.domdocument.6.0", OASP_COMPONENT_XML, "MSXML2.DOMDocument"},
    {"msxml2.freethreadeddomdocument", OASP_COMPONENT_XML, "MSXML2.FreeThreadedDOMDocument"},
    {"msxml2.freethreadeddomdocument.3.0", OASP_COMPONENT_XML, "MSXML2.FreeThreadedDOMDocument"},
    {"msxml2.freethreadeddomdocument.6.0", OASP_COMPONENT_XML, "MSXML2.FreeThreadedDOMDocument"},
    {"microsoft.xmlhttp", OASP_COMPONENT_HTTP, "Microsoft.XMLHTTP"},
    {"msxml2.xmlhttp", OASP_COMPONENT_HTTP, "MSXML2.XMLHTTP"},
    {"msxml2.xmlhttp.3.0", OASP_COMPONENT_HTTP, "MSXML2.XMLHTTP"},
    {"msxml2.xmlhttp.6.0", OASP_COMPONENT_HTTP, "MSXML2.XMLHTTP"},
    {"msxml2.serverxmlhttp", OASP_COMPONENT_HTTP, "MSXML2.ServerXMLHTTP"},
    {"msxml2.serverxmlhttp.3.0", OASP_COMPONENT_HTTP, "MSXML2.ServerXMLHTTP"},
    {"msxml2.serverxmlhttp.6.0", OASP_COMPONENT_HTTP, "MSXML2.ServerXMLHTTP"},
    {"openasp.httpclient", OASP_COMPONENT_HTTP, "OpenASP.HttpClient"},
    {"adodb.connection", OASP_COMPONENT_ADO_CONNECTION, "ADODB.Connection"},
    {"adodb.recordset", OASP_COMPONENT_ADO_RECORDSET, "ADODB.Recordset"},
    {"adodb.command", OASP_COMPONENT_ADO_COMMAND, "ADODB.Command"},
    {"openasp.socket", OASP_COMPONENT_SOCKET, "OpenASP.Socket"},
    {"openasp.redis", OASP_COMPONENT_REDIS, "OpenASP.Redis"},
    {"redis.client", OASP_COMPONENT_REDIS, "OpenASP.Redis"},
    {"openasp.json", OASP_COMPONENT_JSON, "OpenASP.JSON"},
    {"openasp.json.object", OASP_COMPONENT_DICTIONARY, "OpenASP.JSON.Object"},
    {"openasp.crypto", OASP_COMPONENT_CRYPTO, "OpenASP.Crypto"},
    {"openasp.openai", OASP_COMPONENT_OPENAI, "OpenASP.OpenAI"},
    {"openasp.process", OASP_COMPONENT_PROCESS, "OpenASP.Process"},
    {"openasp.websocket", OASP_COMPONENT_WEBSOCKET, "OpenASP.WebSocket"},
    {"openasp.memcache", OASP_COMPONENT_MEMCACHE, "OpenASP.Memcache"},
    {"openasp.memcached", OASP_COMPONENT_MEMCACHE, "OpenASP.Memcache"},
    {"shell.application", OASP_COMPONENT_SHELL, "Shell.Application"},
};

const oasp_component_registration *oasp_builtin_component_registration(oasp_slice prog_id) {
    for (size_t i = 0; i < sizeof(registry) / sizeof(registry[0]); i++)
        if (oasp_ascii_equal(prog_id, registry[i].name))
            return &registry[i];
    return NULL;
}

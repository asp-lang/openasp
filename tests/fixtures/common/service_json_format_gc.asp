<%
' Copyright (c) 2026 https://asp-lang.org
' SPDX-License-Identifier: MIT

Option Explicit

Dim json, value, encoded, index
Set json = Server.CreateObject("OpenASP.JSON")
Set value = Server.CreateObject("Scripting.Dictionary")
value.Add "ok", True
value.Add "request_id", 0
value.Add "worker", 7
value.Add "checksum", 0
value.Add "progress", 0
value.Add "endpoint_code", 5
value.Add "reply_items", 40
value.Add "items", Array("alpha", "beta", "gamma", "delta")

For index = 1 To 100000
    value("request_id") = index
    value("checksum") = (index * 17) Mod 1000003
    value("progress") = index * 1000
    encoded = json.Stringify(value)
    If Len(encoded) < 100 Then
        Response.Write "json-format-fail"
        Response.End
    End If
Next

Response.Write index
%>

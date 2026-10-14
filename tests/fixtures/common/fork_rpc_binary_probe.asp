<%
' Copyright (c) 2026 https://asp-lang.org
' SPDX-License-Identifier: MIT

Option Explicit

Dim payload, middleValue, middleCode, capturedError

payload = "A" & ChrB(0) & "BC"
On Error Resume Next
middleValue = MidB(payload, 2, 1)
middleCode = AscB(middleValue)
capturedError = Err.Number
Err.Clear
On Error GoTo 0

Response.Write "binary-probe|lenb=" & CStr(LenB(payload))
Response.Write "|midb-len=" & CStr(LenB(middleValue))
Response.Write "|midb-code=" & CStr(middleCode)
Response.Write "|error=" & CStr(capturedError)
%>

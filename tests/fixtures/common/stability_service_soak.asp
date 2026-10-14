<%
' Copyright (c) 2026 OpenASP.dev
' SPDX-License-Identifier: MIT

Option Explicit

Class SoakNode
    Public NextNode
    Public Payload
    Public Sequence
End Class

Function Mix(ByVal value)
    value = ((value * 1103515245) + 12345) Mod 1000003
    Mix = (value Xor (value \ 13)) And &H7FFFFFFF
End Function

Dim durationSeconds, started, nowValue, checksum, iterations, slotIndex
Dim values(), head, tail, garbage, textValue

durationSeconds = 10
If IsNumeric(Request.QueryString("duration")) Then durationSeconds = CLng(Request.QueryString("duration"))
If durationSeconds < 1 Then durationSeconds = 1
If durationSeconds > 21600 Then durationSeconds = 21600

ReDim values(127)
Set head = New SoakNode
Set tail = New SoakNode
Set head.NextNode = tail
Set tail.NextNode = head
started = Timer()
checksum = 97
iterations = 0

Do
    iterations = iterations + 1
    slotIndex = iterations Mod (UBound(values) + 1)
    values(slotIndex) = Mix(checksum + iterations + slotIndex)
    checksum = (checksum + values(slotIndex) + slotIndex) Mod 1000003

    Set garbage = New SoakNode
    garbage.Sequence = iterations
    garbage.Payload = String((iterations Mod 64) + 16, Chr(65 + (iterations Mod 26)))
    Set garbage.NextNode = garbage

    If (iterations Mod 251) = 0 Then
        tail.Sequence = iterations
        tail.Payload = CStr(checksum) & ":" & garbage.Payload
        textValue = CStr(iterations) & ":" & CStr(slotIndex) & ":" & CStr(checksum)
        If Len(textValue) = 0 Then
            Response.Write "stability-soak-fail:text"
            Response.End
        End If
    End If

    nowValue = Timer()
Loop While nowValue - started < durationSeconds

If head.NextNode.Sequence <> tail.Sequence Then
    Response.Write "stability-soak-fail:graph"
    Response.End
End If

Response.Write "stability-soak-ok|" & CStr(durationSeconds) & "|" & CStr(iterations) & "|" & CStr(checksum)
%>

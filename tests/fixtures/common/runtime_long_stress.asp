<%
' Copyright (c) 2026 https://asp-lang.org
' SPDX-License-Identifier: MIT

Class StressNode
    Public NextNode
    Public Payload
    Public Sequence
End Class

Function RotateValue(ByVal value, ByRef mutations)
    Dim mixed
    mixed = ((value * 17) + 31) Mod 1000003
    If (mixed Mod 2) = 0 Then
        mixed = mixed \ 2
    Else
        mixed = (mixed * 3) + 1
    End If
    mutations = mutations + 1
    RotateValue = mixed
End Function

Function ClassifyValue(ByVal value)
    Select Case value Mod 11
        Case 0, 2, 4, 6, 8, 10
            ClassifyValue = 3
        Case 1, 5, 9
            ClassifyValue = 5
        Case Else
            ClassifyValue = 7
    End Select
End Function

Sub MutateValue(ByRef value, ByVal delta)
    value = value + delta
End Sub

Dim iterations
iterations = 4000
If IsNumeric(Request.QueryString("iterations")) Then
    iterations = CInt(Request.QueryString("iterations"))
End If
If iterations < 1 Then iterations = 1
If iterations > 200000 Then iterations = 200000

Dim values()
ReDim values(31)

Dim i, slotIndex, checksum, mutations, expectedMutations
Dim textValue, expectedText, category
Dim head, tail, garbage
checksum = 97
mutations = 0

Set head = New StressNode
Set tail = New StressNode
head.Sequence = -1
tail.Sequence = -2
Set head.NextNode = tail
Set tail.NextNode = head

For i = 1 To iterations
    If i = 257 Then ReDim Preserve values(63)
    If i = 1025 Then ReDim Preserve values(127)

    slotIndex = i Mod (UBound(values) + 1)
    values(slotIndex) = RotateValue(checksum + i + slotIndex, mutations)
    Call MutateValue(values(slotIndex), ClassifyValue(i))
    checksum = (checksum + values(slotIndex) + (i Mod 97)) Mod 1000003

    textValue = CStr(i) & ":" & CStr(slotIndex) & ":" & CStr(checksum)
    expectedText = CStr(i) & ":" & CStr(slotIndex) & ":" & CStr(checksum)
    If textValue <> expectedText Then
        Response.Write "stress-fail:string:" & CStr(i)
        Response.End
    End If

    category = ClassifyValue(i)
    If category <> 3 And category <> 5 And category <> 7 Then
        Response.Write "stress-fail:select:" & CStr(i)
        Response.End
    End If

    Set garbage = New StressNode
    garbage.Sequence = i
    garbage.Payload = String((i Mod 96) + 32, Chr(65 + (i Mod 26)))
    Set garbage.NextNode = garbage

    If (i Mod 113) = 0 Then
        tail.Payload = CStr(i) & ":" & garbage.Payload
        tail.Sequence = i
    End If
Next

expectedMutations = iterations
If mutations <> expectedMutations Then
    Response.Write "stress-fail:byref:" & CStr(mutations)
    Response.End
End If

On Error Resume Next
Dim invalidValue, capturedError
invalidValue = 1 / 0
capturedError = Err.Number
Err.Clear
On Error GoTo 0
If capturedError = 0 Then
    Response.Write "stress-fail:error-state"
    Response.End
End If

If head.NextNode.Sequence <> tail.Sequence Then
    Response.Write "stress-fail:object-graph"
    Response.End
End If

Response.Write "stress-ok|" & CStr(iterations) & "|" & CStr(checksum) & "|" & CStr(mutations) & "|" & CStr(UBound(values))
%>

<%
' A taken native branch at End Sub must unwind to its caller, including ByRef.
Sub CheckBoundary(ByVal input, ByRef output)
    Dim actual
    actual = input
    output = actual + 1
    If actual <> input Then
        Response.Write "unreachable"
    End If
End Sub

Function CheckFunction(ByVal input)
    Dim actual
    actual = input
    CheckFunction = actual + 2
    If actual <> input Then
        Response.Write "unreachable"
    End If
End Function

Dim output
CheckBoundary 94, output
Response.Write output & "|" & CheckFunction(94) & "|caller-resumed"
%>

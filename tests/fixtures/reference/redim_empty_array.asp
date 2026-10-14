<%
Function BuildEmptyArrayText()
    Dim values()
    ReDim values(-1)

    If Join(values) <> "" Then
        BuildEmptyArrayText = "join-failed"
        Exit Function
    End If

    ReDim Preserve values(0)
    values(0) = "ready"
    BuildEmptyArrayText = values(0)
End Function

Dim topLevel()
ReDim topLevel(-1)
Response.Write "top=" & Join(topLevel) & ";function=" & BuildEmptyArrayText()
%>

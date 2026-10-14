<%
Sub UpdateValues(ByRef changed, ByVal unchanged, ByRef literalValue)
    changed = changed + 1
    unchanged = unchanged + 10
    literalValue = literalValue + 100
    Exit Sub
    changed = 999
End Sub

Dim first
Dim second
first = 1
second = 2
Call UpdateValues(first, second, 5)
Response.Write CStr(first) & ":" & CStr(second)
%>

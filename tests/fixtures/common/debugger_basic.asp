<%
Function AddValue(value)
    Dim local
    local = value + 1
    AddValue = local
End Function

Dim result
result = AddValue(40)
Response.Write result
%>

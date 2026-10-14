<%
Dim value
value=0
If False Then
    value=1
ElseIf True Then
    value=2
Else
    value=3
End If
Response.Write value
%>

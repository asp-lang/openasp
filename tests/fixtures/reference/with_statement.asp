<%
Class Holder
    Public Value
End Class
Dim holder
Set holder = New Holder
With holder
    .Value = 7
End With
Response.Write holder.Value
%>

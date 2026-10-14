<%
Class Box
    Public Property Get Value
        Value = 7
        Exit Property
        Value = 9
    End Property
End Class
Set box = New Box
Response.Write CStr(box.Value)
%>

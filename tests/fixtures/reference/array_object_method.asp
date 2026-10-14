<%
Class Box
    Public Value

    Public Sub Store(input)
        Value = input
    End Sub
End Class

Dim boxes(1)
Set boxes(1) = New Box
Call boxes(1).Store("updated")
Response.Write CStr(IsObject(boxes(1))) & "|" & boxes(1).Value
%>

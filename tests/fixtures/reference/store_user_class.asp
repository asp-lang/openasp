<%
Class Box
    Dim Value

    Function Peek()
        Peek = Value
    End Function
End Class

Set box = New Box
box.Value = "persisted"
Session("box") = box
Response.Write "stored"
%>

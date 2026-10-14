<%
Class Box
    Dim Value

    Function Peek()
        Peek = Value
    End Function
End Class

Response.Write Session("box").Peek()
Response.Write "|"
Response.Write Session("box").Value
%>

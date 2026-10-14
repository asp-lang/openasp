<%
Dim name
name = Request("name")
If name = "" Then
    Response.Write "guest"
Else
    Response.Write name
End If
%>

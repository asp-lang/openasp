<%
Dim key
For Each key In Request.Form
    Response.Write key & "=" & Request.Form(key) & ";"
Next
%>

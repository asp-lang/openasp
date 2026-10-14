<%
Response.ContentType = "text/plain"
total = 0
For i = 1 To 2000
    total = total + Eval("20 + 2")
Next
Response.Write total
%>

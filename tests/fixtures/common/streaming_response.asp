<%
Response.Write "discard-before-flush"
Response.Clear
Response.Write "A"
Response.Flush
Response.Write "discard-after-flush"
Response.Clear
Response.Buffer = False
Response.Write "B"
%>

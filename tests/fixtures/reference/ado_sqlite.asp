<%
Set conn = Server.CreateObject("ADODB.Connection")
Call conn.Open("Provider=SQLite;Data Source=" & Request("db"))
Set rs = conn.Execute("select id, name from demo order by id")
Response.Write rs.Fields("id")
Response.Write "|"
Response.Write rs.Fields("name")
Response.Write "|"
rows = rs.GetRows()
Response.Write rows(1, 1)
%>

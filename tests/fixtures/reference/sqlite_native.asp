<%
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=SQLite;Data Source=" & Request("db"))
Call connection.Execute("drop table if exists demo; create table demo(id integer primary key, name text); insert into demo values(1, 'alpha'), (2, 'beta')")
Set rows = connection.Execute("select id, name from demo order by id")
Response.Write rows.Fields("id")
Response.Write ":"
Response.Write rows.Fields("name")
Call rows.MoveNext()
Response.Write "|"
Response.Write rows.Fields("id")
Response.Write ":"
Response.Write rows.Fields("name")
%>

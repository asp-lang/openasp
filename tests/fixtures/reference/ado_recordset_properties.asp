<%
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=SQLite;Data Source=" & Request("db"))
Call connection.Execute("drop table if exists demo; create table demo(id integer primary key, name text); insert into demo values(1, 'alpha')")
Set rows = Server.CreateObject("ADODB.Recordset")
rows.CursorType = 1
rows.LockType = 3
Set rows.ActiveConnection = connection
Call rows.Open("select id, name from demo")
Response.Write rows.CursorType & "|" & rows.LockType & "|" & rows.Fields("name")
Set rows.ActiveConnection = Nothing
Response.Write "|" & IsObject(rows.ActiveConnection)
%>

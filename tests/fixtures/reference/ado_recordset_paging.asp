<%
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=SQLite;Data Source=" & Request("db"))
Call connection.Execute("drop table if exists paging_demo; create table paging_demo(id integer primary key); insert into paging_demo values(1), (2), (3), (4), (5), (6)")
Set rows = connection.Execute("select id from paging_demo order by id")
rows.PageSize = 2
rows.AbsolutePage = 2
Response.Write rows.PageSize
Response.Write "|"
Response.Write rows.AbsolutePage
Response.Write "|"
Response.Write rows.PageCount
Response.Write "|"
Response.Write rows.RecordCount
Response.Write "|"
Response.Write rows.Fields("id")
Call rows.MoveNext()
Response.Write "|"
Response.Write rows.Fields("id")
%>

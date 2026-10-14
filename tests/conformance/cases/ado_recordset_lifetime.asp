<%
Dim connectionString, recordset, saved
connectionString = "Provider=SQLite;Data Source=" & Request.QueryString("db")
Set recordset = Server.CreateObject("ADODB.Recordset")
recordset.Open "create table demo(id integer primary key, name text);insert into demo values(1, 'alpha')", connectionString, 1, 3
recordset.Open "select id, name from demo where id = 1", connectionString, 1, 1
saved = recordset(1)
recordset.Close
recordset.Open "update demo set name = 'beta' where id = 1", connectionString, 1, 3
Response.Write saved
%>

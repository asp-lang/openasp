<%
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=SQLite;Data Source=" & Request("db"))
Call connection.Execute("drop table if exists eof_demo; create table eof_demo(id integer primary key); insert into eof_demo values(1)")
Set rows = connection.Execute("select id from eof_demo")
count = 0
For outer = 1 To 1
    Do While Not rows.EOF And count < 4
        count = count + 1
        Call rows.MoveNext()
    Loop
Next
Response.Write count
%>

<%
Dim connection, rows
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=OpenASP.MySQL;Server=127.0.0.1;Port=" & Request("port") & ";User Id=root;Password=;Database=test;Charset=utf8mb4")
Set rows = connection.Execute("SELECT 1 AS value UNION ALL SELECT 2 UNION ALL SELECT 3")
Do While Not rows.EOF
    Response.Write CStr(rows.Fields("value"))
    rows.MoveNext
Loop
rows.Close
connection.Close
%>

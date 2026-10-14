<%
Dim connection, outerRows, innerRows
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=OpenASP.MySQL;Server=127.0.0.1;Port=" & Request("port") & ";User Id=root;Password=;Database=test;Charset=utf8mb4;BufferedResult=True")
Set outerRows = connection.Execute("SELECT 1 AS value UNION ALL SELECT 2 UNION ALL SELECT 3")
Do While Not outerRows.EOF
    Response.Write CStr(outerRows.Fields("value"))
    Set innerRows = connection.Execute("SELECT 9 AS nested_value")
    Response.Write CStr(innerRows.Fields("nested_value"))
    innerRows.Close
    outerRows.MoveNext
Loop
outerRows.Close
connection.Close
%>

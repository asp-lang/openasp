<%
Dim connection, recordset, rows, buffered
buffered = Request("buffered")

Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=OpenASP.MySQL;Server=127.0.0.1;Port=" & Request("port") & ";User Id=root;Password=;Database=test;Charset=utf8mb4;BufferedResult=" & buffered)
Set recordset = connection.Execute("SELECT 'r1-alpha' AS alpha, 'r1-beta' AS beta, 'r1-gamma' AS gamma UNION ALL SELECT 'r2-alpha', 'r2-beta', 'r2-gamma'")

rows = recordset.GetRows()
Response.Write rows(0, 0) & "|" & rows(1, 0) & "|" & rows(2, 0) & "|"
Response.Write rows(0, 1) & "|" & rows(1, 1) & "|" & rows(2, 1) & "|"
Response.Write CStr(recordset.EOF) & "|" & CStr(recordset.BOF)

recordset.Close
connection.Close
%>

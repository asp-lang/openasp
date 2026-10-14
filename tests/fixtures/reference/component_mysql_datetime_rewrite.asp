<%
On Error Resume Next
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=OpenASP.MySQL;Server=127.0.0.1;Port=" & Request("port") & ";User Id=root;Password=;Database=test;Charset=utf8mb4")
If Err.Number <> 0 Then
    Response.Write "open-error:" & Err.Number & ":" & Err.Description
    Response.End
End If

Call connection.Execute("CREATE TEMPORARY TABLE egret_datetime_rewrite (created_at DATETIME NOT NULL, label VARCHAR(64) NOT NULL)")
Call connection.Execute("INSERT INTO egret_datetime_rewrite (created_at, label) VALUES ('9/24/2026 5:06:07', 'release 9/24/2026 note')")
Set rows = connection.Execute("SELECT DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS normalized, label, '2/29/2025' AS invalid_date FROM egret_datetime_rewrite")
If Err.Number <> 0 Then
    Response.Write "query-error:" & Err.Number & ":" & Err.Description
Else
    Response.Write rows.Fields("normalized")
    Response.Write "|"
    Response.Write rows.Fields("label")
    Response.Write "|"
    Response.Write rows.Fields("invalid_date")
End If

Set command = Server.CreateObject("ADODB.Command")
Set command.ActiveConnection = connection
command.CommandText = "SELECT DATE_FORMAT(CAST('2/29/2024 23:59:58' AS DATETIME), '%Y-%m-%d %H:%i:%s') AS prepared_date, ? AS marker"
Set parameter = command.CreateParameter("marker", 200, 1, 16, "prepared")
Call command.Parameters.Append(parameter)
Set preparedRows = command.Execute()
Response.Write "|"
If Err.Number <> 0 Then
    Response.Write "prepare-error:" & Err.Number & ":" & Err.Description
Else
    Response.Write preparedRows.Fields("prepared_date")
    Response.Write "|"
    Response.Write preparedRows.Fields("marker")
End If
Call connection.Close()
%>

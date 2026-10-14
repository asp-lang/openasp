<%
On Error Resume Next
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=OpenASP.MySQL;Server=127.0.0.1;Port=" & Request("port") & ";User Id=root;Password=;Database=test;Charset=utf8mb4;Connection Timeout=3")
If Err.Number <> 0 Then
    Response.Write "open-error:" & Err.Number & ":" & Err.Description
    Response.End
End If
Response.Write connection.State
Response.Write ":"
Set rows = connection.Execute("SELECT 'plain' AS kind, 41 AS value")
Response.Write rows.Fields("kind")
Response.Write ":"
Response.Write rows.Fields("value")
Response.Write ":"
Set command = Server.CreateObject("ADODB.Command")
Set command.ActiveConnection = connection
command.CommandText = "SELECT ? AS text_value, ? + 1 AS number_value"
Set textParameter = command.CreateParameter("text_value", 200, 1, 64, "bound")
Set numberParameter = command.CreateParameter("number_value", 3, 1, 0, 41)
Set parameters = command.Parameters
Call parameters.Append(textParameter)
Call parameters.Append(numberParameter)
Set preparedRows = command.Execute()
If Err.Number <> 0 Then
    Response.Write "command-error:" & Err.Number & ":" & Err.Description
    Response.End
End If
Response.Write preparedRows.Fields("text_value")
Response.Write ":"
Response.Write preparedRows.Fields("number_value")
Call connection.Close()
Response.Write ":"
Response.Write connection.State
%>

<%
On Error Resume Next
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=OpenASP.MySQL;Server=127.0.0.1;Port=" & Request("port") & ";User Id=root;Password=;Database=test;Charset=utf8mb4")
If Err.Number <> 0 Then
    Response.Write "open-error:" & Err.Number & ":" & Err.Description
    Response.End
End If
Set rows = connection.Execute("SELECT CONNECTION_ID() AS connection_id UNION ALL SELECT CONNECTION_ID() UNION ALL SELECT CONNECTION_ID()")
If Err.Number <> 0 Then
    Response.Write "query-error:" & Err.Number & ":" & Err.Description
Else
    Response.Write rows.Fields("connection_id")
End If
Call connection.Close()
%>

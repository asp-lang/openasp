<%
On Error Resume Next
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=Unregistered.Provider;Server=127.0.0.1;Port=" & Request("port") & ";User Id=root;Password=;Database=test")
If Err.Number <> 0 Then
    Response.Write "open-error:" & Err.Number & ":" & Err.Description
    Response.End
End If
Set rows = connection.Execute("SELECT 'mysql-fallback' AS result")
If Err.Number <> 0 Then
    Response.Write "query-error:" & Err.Number & ":" & Err.Description
Else
    Response.Write rows.Fields("result")
End If
Call connection.Close()
Response.Write ":"
Set defaultConnection = Server.CreateObject("ADODB.Connection")
Call defaultConnection.Open("Server=127.0.0.1;Port=" & Request("port") & ";User Id=root;Password=;Database=test")
If Err.Number <> 0 Then
    Response.Write "default-open-error:" & Err.Number & ":" & Err.Description
    Response.End
End If
Set defaultRows = defaultConnection.Execute("SELECT 'mysql-default' AS result")
If Err.Number <> 0 Then
    Response.Write "default-query-error:" & Err.Number & ":" & Err.Description
Else
    Response.Write defaultRows.Fields("result")
End If
Call defaultConnection.Close()
%>

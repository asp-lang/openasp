<%
On Error Resume Next
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=OpenASP.MySQL;Server=127.0.0.1;Port=" & Request("port") & ";User Id=root;Password=;Database=test;Charset=utf8mb4;Connection Timeout=3")
If Err.Number <> 0 Then
    Response.Write "open-error:" & Err.Number & ":" & Err.Description
    Response.End
End If
Set rows = connection.Execute("SELECT 1 AS value FROM information_schema.COLUMNS LIMIT 200")
total = 0
count = 0
Do While Not rows.EOF
    total = total + rows.Fields("value")
    count = count + 1
    rows.MoveNext
Loop
Response.Write total & ":" & count & ":" & rows.RecordCount & ":" & rows.EOF
Call connection.Close()
%>

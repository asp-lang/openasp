<%
On Error Resume Next
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=OpenASP.MySQL;Server=127.0.0.1;Port=" & Request("port") & ";User Id=root;Password=;Database=" & Request("database") & ";Charset=utf8mb4")
If Err.Number <> 0 Then
    Response.Write "open-error:" & Err.Number & ":" & Err.Description
    Response.End
End If

Set emptyRows = connection.Execute("SELECT tag_ID AS value FROM blog_Tag WHERE 1=0")
If Err.Number <> 0 Then
    Response.Write "empty-query-error:" & Err.Number & ":" & Err.Description
    Response.End
End If
Response.Write CStr(IsObject(emptyRows)) & "|" & CStr(emptyRows.BOF) & "|" & CStr(emptyRows.EOF)

Set rows = connection.Execute("SELECT 1 AS value")
If Err.Number <> 0 Then
    Response.Write "|row-query-error:" & Err.Number & ":" & Err.Description
Else
    Response.Write "|" & rows("value")
End If
Call connection.Close()
%>

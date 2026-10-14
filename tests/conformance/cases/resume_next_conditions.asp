<%
On Error Resume Next
Dim obj, hits
Set obj = Nothing
hits = 0
If Not obj.eof Then hits = hits + 1
Response.Write "if:" & hits & ":" & Err.Number & "|"
Err.Clear
Do While Not obj.eof
    hits = hits + 1
    If hits > 3 Then Exit Do
Loop
Response.Write "loop:" & hits & ":" & Err.Number
Err.Clear
If obj.eof Then
    hits = hits + 10
Else
    hits = hits + 20
End If
While Not obj.eof
    hits = hits + 1
    If hits > 3 Then Response.End
Wend
Execute "If Not obj.eof Then hits = hits + 100"
Response.Write "|branches:" & hits & ":" & Err.Number
Err.Clear
Do
    hits = hits + 1
    If hits > 3 Then Exit Do
Loop Until obj.eof
Response.Write "|post:" & hits & ":" & Err.Number
Dim missing, preserved
preserved = "kept"
Err.Clear
preserved = missing.Execute("ignored")
Response.Write "|empty:" & preserved & ":" & Err.Number
%>

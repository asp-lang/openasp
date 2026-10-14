<%
Dim value

If Request.QueryString("read") = "1" Then
    Response.Write CStr(Application("cross_process_counter"))
    Response.End
End If

Application.Lock
value = Application("cross_process_counter")
If IsEmpty(value) Then value = 0

Dim i
For i = 1 To 5000
    value = value + 0
Next

Application("cross_process_counter") = CLng(value) + 1
Response.Write CStr(Application("cross_process_counter"))
Application.Unlock
%>

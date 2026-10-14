<%
On Error Resume Next
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=OpenASP.MySQL;Server=127.0.0.1;Port=" & Request("port") & ";User Id=root;Password=;Database=test")
Set command = Server.CreateObject("ADODB.Command")
Set command.ActiveConnection = connection
command.CommandText = "SELECT ? AS text_value"
Set parameter = command.CreateParameter("text_value", 200, 1, 64, "bound")
Set parameters = command.Parameters
Call parameters.Append(parameter)
Set rows = command.Execute()
If Err.Number <> 0 Then
    Response.Write "error:" & Err.Number & ":" & Err.Description
Else
    Response.Write rows.Fields("text_value")
End If
Call connection.Close()
%>

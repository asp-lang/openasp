<%
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=SQLite;Data Source=" & Request("db"))
Set command = Server.CreateObject("ADODB.Command")
Set command.ActiveConnection = connection
command.CommandText = "select id, name from demo where id = ?"
Set parameter = command.CreateParameter("id", 3, 1, 0, 2)
Set parameters = command.Parameters
Call parameters.Append(parameter)
Set rows = command.Execute()
Response.Write rows.Fields("id")
Response.Write ":"
Response.Write rows.Fields("name")
Response.Write ":"
Response.Write parameters.Count
%>

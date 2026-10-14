<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request.QueryString("db")
Set recordset = connection.Execute("SELECT TOP 1 ActCMS_Theme FROM [Config_Act]")
Response.Write recordset("ActCMS_Theme")
%>

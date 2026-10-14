<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
Set recordset = Server.CreateObject("ADODB.Recordset")
recordset.Open "SELECT * FROM Admin_ACT WHERE Admin_Name='admin'", connection, 1, 3
recordset("LoginIP") = "127.0.0.2"
recordset.Update
Response.Write recordset("LoginIP")
%>

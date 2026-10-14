<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")

connection.Execute "INSERT INTO Admin_Act (Admin_Name, Password, Locked, SuperTF) VALUES ('mdb-execute-delete', 'test', 0, 0)"
Set recordset = Server.CreateObject("ADODB.Recordset")
recordset.Open "SELECT * FROM Admin_Act WHERE Admin_Name='mdb-execute-delete'", connection, 1, 3
recordset.Delete

connection.Execute "UPDATE Admin_Act SET LoginIP='10.20.30.40' WHERE Admin_Name='admin'"
connection.BeginTrans
connection.Execute "UPDATE Admin_Act SET LoginIP='10.20.30.41' WHERE Admin_Name='admin'"
connection.RollbackTrans

Set recordset = connection.Execute("SELECT LoginIP FROM Admin_Act WHERE Admin_Name='admin'")
Response.Write recordset("LoginIP")
%>

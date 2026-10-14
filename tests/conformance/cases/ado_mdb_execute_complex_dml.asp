<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")

connection.Execute "INSERT INTO Admin_Act (Admin_Name, Password, Locked, SuperTF) VALUES ('mdb-complex-1', 'seed', 0, 0)"
connection.Execute "INSERT INTO Admin_Act (Admin_Name, Password, Locked, SuperTF) VALUES ('mdb-complex-2', 'seed', 0, 0)"
connection.Execute "INSERT INTO Admin_Act (Admin_Name, Password, Locked, SuperTF) VALUES ('mdb-complex-3', 'seed', 1, 0)"
connection.Execute "UPDATE Admin_Act SET Password=Password & '-ok' WHERE Admin_Name IN ('mdb-complex-1', 'mdb-complex-2') AND (Locked=0 OR LoginIP IS NULL)"

Set recordset = connection.Execute("SELECT Password FROM Admin_Act WHERE Admin_Name='mdb-complex-1'")
Response.Write recordset("Password") & ":"

connection.Execute "DELETE FROM Admin_Act WHERE Admin_Name IN ('mdb-complex-1', 'mdb-complex-2') AND Password='seed-ok'"
Set recordset = connection.Execute("SELECT Password FROM Admin_Act WHERE Admin_Name='mdb-complex-3'")
Response.Write recordset("Password")
%>

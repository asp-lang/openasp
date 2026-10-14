<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")

connection.BeginTrans
connection.Execute "INSERT INTO Admin_Act (Admin_Name, Password, Locked, SuperTF) VALUES ('oasp-transaction-rollback-a', 'one', 0, 0)"
connection.Execute "INSERT INTO Admin_Act (Admin_Name, Password, Locked, SuperTF) VALUES ('oasp-transaction-rollback-b', 'two', 0, 0)"
connection.RollbackTrans

connection.BeginTrans
connection.Execute "INSERT INTO Admin_Act (Admin_Name, Password, Locked, SuperTF) VALUES ('oasp-transaction-commit-a', 'one', 0, 0)"
connection.Execute "UPDATE Admin_Act SET Password='two' WHERE Admin_Name='oasp-transaction-commit-a'"
connection.CommitTrans

Set recordset = connection.Execute("SELECT Admin_Name FROM Admin_Act WHERE Admin_Name='oasp-transaction-rollback-a'")
If recordset.EOF Then Response.Write "rolled:"
Set recordset = connection.Execute("SELECT Password FROM Admin_Act WHERE Admin_Name='oasp-transaction-commit-a'")
Response.Write recordset(0)
%>

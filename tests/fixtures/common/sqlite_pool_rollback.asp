<%
Dim connection
Dim recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=SQLite;Data Source=" & Request("db")
connection.Execute "CREATE TABLE IF NOT EXISTS pool_test(value INTEGER)"
connection.Execute "DELETE FROM pool_test"
connection.BeginTrans
connection.Execute "INSERT INTO pool_test(value) VALUES(1)"
connection.Close
connection.Open "Provider=SQLite;Data Source=" & Request("db")
Set recordset = connection.Execute("SELECT COUNT(*) AS total FROM pool_test")
Response.Write recordset("total")
connection.Close
%>

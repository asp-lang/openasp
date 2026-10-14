<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspSubqueryTest (Value INTEGER)"
connection.Execute "INSERT INTO OaspSubqueryTest (Value) VALUES (1)"
connection.Execute "INSERT INTO OaspSubqueryTest (Value) VALUES (2)"
Set recordset = connection.Execute("SELECT Value FROM OaspSubqueryTest WHERE Value IN (SELECT Value FROM OaspSubqueryTest WHERE Value=2)")
Response.Write recordset(0)
connection.Execute "DROP TABLE OaspSubqueryTest"
%>

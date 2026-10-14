<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspCorrelatedTest (Value INTEGER)"
connection.Execute "INSERT INTO OaspCorrelatedTest (Value) VALUES (1)"
connection.Execute "INSERT INTO OaspCorrelatedTest (Value) VALUES (2)"
Set recordset = connection.Execute("SELECT o.Value FROM OaspCorrelatedTest AS o WHERE EXISTS (SELECT i.Value FROM OaspCorrelatedTest AS i WHERE i.Value=o.Value AND i.Value=2)")
Response.Write recordset(0) & ":"
Set recordset = connection.Execute("SELECT d.Value FROM (SELECT Value FROM OaspCorrelatedTest WHERE Value=2) AS d")
Response.Write recordset(0)
connection.Execute "DROP TABLE OaspCorrelatedTest"
%>

<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspUnionTest (Value INTEGER)"
connection.Execute "INSERT INTO OaspUnionTest (Value) VALUES (1)"
connection.Execute "INSERT INTO OaspUnionTest (Value) VALUES (2)"
Set recordset = connection.Execute("SELECT Value FROM OaspUnionTest WHERE Value=1 UNION SELECT Value FROM OaspUnionTest WHERE Value=1")
Response.Write recordset(0) & ":"
recordset.MoveNext
Response.Write CStr(recordset.EOF)
Set recordset = connection.Execute("SELECT Value FROM OaspUnionTest WHERE Value=1 UNION ALL SELECT Value FROM OaspUnionTest WHERE Value=1")
Response.Write ":" & recordset(0)
recordset.MoveNext
Response.Write ":" & recordset(0)
connection.Execute "DROP TABLE OaspUnionTest"
%>

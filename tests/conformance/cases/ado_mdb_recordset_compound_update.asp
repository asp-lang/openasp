<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspRecordsetCompoundUpdate (id LONG, scope TEXT(20), value TEXT(20))"
connection.Execute "INSERT INTO OaspRecordsetCompoundUpdate (id, scope, value) VALUES (1, 'alpha', 'old')"
Set recordset = Server.CreateObject("ADODB.Recordset")
recordset.Open "SELECT * FROM OaspRecordsetCompoundUpdate WHERE id=1 AND scope='alpha'", connection, 1, 3
recordset("value") = "updated"
recordset.Update
Response.Write recordset("value")
connection.Execute "DROP TABLE OaspRecordsetCompoundUpdate"
%>

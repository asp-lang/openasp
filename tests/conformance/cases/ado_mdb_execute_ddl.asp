<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspDdlTest (ID AUTOINCREMENT PRIMARY KEY, Label TEXT(32))"
connection.Execute "INSERT INTO OaspDdlTest (Label) VALUES ('created')"
connection.Execute "ALTER TABLE OaspDdlTest ALTER COLUMN Label TEXT(64)"
connection.Execute "ALTER TABLE OaspDdlTest ADD COLUMN Note TEXT(32)"
connection.Execute "ALTER TABLE OaspDdlTest ADD COLUMN Score INTEGER"
connection.Execute "UPDATE OaspDdlTest SET Note='altered' WHERE Label='created'"
connection.Execute "UPDATE OaspDdlTest SET Score=42 WHERE Label='created'"
connection.Execute "INSERT INTO OaspDdlTest (Label) VALUES ('empty')"
connection.Execute "ALTER TABLE OaspDdlTest ALTER COLUMN Score TEXT(16)"
connection.Execute "ALTER TABLE OaspDdlTest ALTER COLUMN Score DOUBLE"
connection.Execute "ALTER TABLE OaspDdlTest DROP COLUMN Note"
Set recordset = connection.Execute("SELECT Label, Score FROM OaspDdlTest WHERE Label='created'")
Response.Write recordset("Label") & ":" & recordset("Score")
Set recordset = connection.Execute("SELECT Score FROM OaspDdlTest WHERE Label='empty'")
Response.Write ":" & CStr(IsNull(recordset("Score")))
connection.Execute "DROP TABLE OaspDdlTest"
connection.Execute "CREATE TABLE OaspDdlTest (Value INTEGER)"
connection.Execute "INSERT INTO OaspDdlTest (Value) VALUES (7)"
Set recordset = connection.Execute("SELECT Value FROM OaspDdlTest")
Response.Write ":" & recordset("Value")
connection.Execute "DROP TABLE OaspDdlTest"
connection.Execute "DROP TABLE IF EXISTS OaspDdlTest"
%>

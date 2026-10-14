<%
Dim connection, recordset, duplicateRejected, compositeRejected
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspIndexDdlTest (Value INTEGER, Label TEXT(16))"
connection.Execute "INSERT INTO OaspIndexDdlTest (Value, Label) VALUES (1, 'alpha')"
connection.Execute "INSERT INTO OaspIndexDdlTest (Value, Label) VALUES (2, 'beta')"
connection.Execute "CREATE UNIQUE INDEX OaspIndexDdlValue ON OaspIndexDdlTest (Value)"
On Error Resume Next
connection.Execute "INSERT INTO OaspIndexDdlTest (Value, Label) VALUES (1, 'alpha')"
duplicateRejected = (Err.Number <> 0)
Err.Clear
On Error GoTo 0
connection.Execute "DROP INDEX OaspIndexDdlValue"
connection.Execute "INSERT INTO OaspIndexDdlTest (Value, Label) VALUES (1, 'gamma')"
connection.Execute "CREATE INDEX OaspIndexDdlValue ON OaspIndexDdlTest (Value)"
connection.Execute "DROP INDEX OaspIndexDdlValue"
connection.Execute "CREATE UNIQUE INDEX OaspIndexDdlComposite ON OaspIndexDdlTest (Value, Label)"
connection.Execute "INSERT INTO OaspIndexDdlTest (Value, Label) VALUES (1, 'beta')"
On Error Resume Next
connection.Execute "INSERT INTO OaspIndexDdlTest (Value, Label) VALUES (1, 'beta')"
compositeRejected = (Err.Number <> 0)
Err.Clear
On Error GoTo 0
connection.Execute "INSERT INTO OaspIndexDdlTest (Label) VALUES ('beta')"
connection.Execute "INSERT INTO OaspIndexDdlTest (Label) VALUES ('beta')"
connection.Execute "DROP INDEX OaspIndexDdlComposite"
Set recordset = connection.Execute("SELECT COUNT(*) FROM OaspIndexDdlTest")
Response.Write CStr(duplicateRejected) & ":" & CStr(compositeRejected) & ":" & recordset(0)
connection.Execute "DROP TABLE OaspIndexDdlTest"
%>

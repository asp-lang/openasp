<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspSelectLeft (ID INTEGER, Name TEXT(16), Score INTEGER, Note TEXT(16))"
connection.Execute "CREATE TABLE OaspSelectRight (ID INTEGER, Label TEXT(16))"
connection.Execute "INSERT INTO OaspSelectLeft (ID, Name, Score, Note) VALUES (1, 'alpha', 1, 'x')"
connection.Execute "INSERT INTO OaspSelectLeft (ID, Name, Score) VALUES (2, 'alpha', 2)"
connection.Execute "INSERT INTO OaspSelectLeft (ID, Name, Score, Note) VALUES (3, 'beta', 3, 'y')"
connection.Execute "INSERT INTO OaspSelectRight (ID, Label) VALUES (1, 'one')"

Set recordset = connection.Execute("SELECT DISTINCT Name FROM OaspSelectLeft ORDER BY Name")
Response.Write recordset(0) & ":"
recordset.MoveNext
Response.Write recordset(0) & ":"

Set recordset = connection.Execute("SELECT COUNT(*) FROM OaspSelectLeft WHERE Name LIKE 'a*' OR Score BETWEEN 3 AND 3")
Response.Write recordset(0) & ":"
Set recordset = connection.Execute("SELECT COUNT(*) FROM OaspSelectLeft WHERE ID NOT IN (1, 3) OR Note IS NOT NULL")
Response.Write recordset(0) & ":"
Set recordset = connection.Execute("SELECT COUNT(*) FROM OaspSelectLeft WHERE ID NOT IN (1, 3) AND Note IS NOT NULL")
Response.Write recordset(0) & ":"
Set recordset = connection.Execute("SELECT COUNT(*) FROM OaspSelectLeft WHERE (ID=1 OR ID=2) AND Note IS NULL")
Response.Write recordset(0) & ":"
Set recordset = connection.Execute("SELECT UCase(Name), Len(Name), Left(Name, 2), Mid(Name, 2, 2), Nz(Note, 'none') FROM OaspSelectLeft WHERE ID=2")
Response.Write recordset(0) & ":" & recordset(1) & ":" & recordset(2) & ":" & recordset(3) & ":" & recordset(4) & ":"
Set recordset = connection.Execute("SELECT l.ID, r.Label FROM OaspSelectLeft AS l LEFT JOIN OaspSelectRight AS r ON l.ID=r.ID WHERE l.ID BETWEEN 1 AND 2 ORDER BY l.ID")
Response.Write recordset(0) & ":" & recordset(1) & ":"
recordset.MoveNext
Response.Write recordset(0) & ":" & CStr(IsNull(recordset(1)))
Set recordset = connection.Execute("SELECT COUNT(*) FROM OaspSelectLeft AS l LEFT JOIN (SELECT ID, Label FROM OaspSelectRight) AS r ON l.ID=r.ID WHERE r.Label IS NULL")
Response.Write ":" & recordset(0)
connection.Execute "INSERT INTO OaspSelectRight (ID, Label) SELECT ID, Note FROM OaspSelectLeft WHERE (ID=1 OR ID=2) AND Score BETWEEN 1 AND 2"
Set recordset = connection.Execute("SELECT COUNT(*) FROM OaspSelectRight WHERE Label IS NULL")
Response.Write ":" & recordset(0)
connection.Execute "DROP TABLE OaspSelectRight"
connection.Execute "DROP TABLE OaspSelectLeft"
%>

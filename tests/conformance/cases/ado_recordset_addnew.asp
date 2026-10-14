<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=SQLite;Data Source=:memory:"
connection.Execute "CREATE TABLE entries (title TEXT, rank INTEGER)"
Set recordset = Server.CreateObject("ADODB.Recordset")
recordset.Open "SELECT title, rank FROM entries", connection, 1, 3
recordset.AddNew
recordset("title") = "first"
recordset("rank") = 7
recordset.Update
Response.Write recordset("title") & ":" & recordset("rank") & ":" & recordset.RecordCount
%>

<%
Dim connection, recordset, field, names
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=SQLite;Data Source=:memory:"
connection.Execute "CREATE TABLE entries (title TEXT, rank INTEGER)"
connection.Execute "INSERT INTO entries VALUES ('first', 7)"
Set recordset = connection.Execute("SELECT title, rank FROM entries")
For Each field In recordset.Fields
  names = names & field.Name & ":" & field.Value & "|"
Next
Response.Write recordset.Fields.Count & ";" & names
%>

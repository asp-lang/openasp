<%
Dim connection, recordset, value, i
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspMemoUpdateTest (Body MEMO)"
value = ""
For i = 1 To 3000
    value = value & "x"
Next
connection.Execute "INSERT INTO OaspMemoUpdateTest (Body) VALUES ('" & value & "')"
connection.Execute "UPDATE OaspMemoUpdateTest SET Body = '" & value & "y'"
Set recordset = connection.Execute("SELECT Body FROM OaspMemoUpdateTest")
Response.Write Len(recordset(0))
connection.Execute "DROP TABLE OaspMemoUpdateTest"
%>

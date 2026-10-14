<%
Dim connection, recordset, value, i
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspMemoTest (Body MEMO)"
value = ""
For i = 1 To 3000
    value = value & "x"
Next
connection.Execute "INSERT INTO OaspMemoTest (Body) VALUES ('" & value & "')"
Set recordset = connection.Execute("SELECT Body FROM OaspMemoTest")
Response.Write Len(recordset(0))
connection.Execute "DROP TABLE OaspMemoTest"
%>

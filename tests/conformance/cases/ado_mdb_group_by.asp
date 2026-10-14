<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspGroupTest (Category TEXT(16), Amount INTEGER)"
connection.Execute "INSERT INTO OaspGroupTest (Category, Amount) VALUES ('a', 2)"
connection.Execute "INSERT INTO OaspGroupTest (Category, Amount) VALUES ('b', 3)"
connection.Execute "INSERT INTO OaspGroupTest (Category, Amount) VALUES ('a', 5)"
Set recordset = connection.Execute("SELECT Category, COUNT(Category), SUM(Amount) FROM OaspGroupTest GROUP BY Category HAVING COUNT(Category) >= 2")
Response.Write recordset(0) & ":" & recordset(1) & ":" & recordset(2)
connection.Execute "DROP TABLE OaspGroupTest"
%>

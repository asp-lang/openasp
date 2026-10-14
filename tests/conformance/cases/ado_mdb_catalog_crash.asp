<%
Dim connection
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspCatalogOne (Value INTEGER)"
connection.Execute "CREATE TABLE OaspCatalogTwo (Value INTEGER)"
connection.Execute "CREATE TABLE OaspCatalogThree (Value INTEGER)"
%>

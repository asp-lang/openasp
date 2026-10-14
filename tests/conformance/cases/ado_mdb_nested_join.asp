<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
connection.Open "Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db")
connection.Execute "CREATE TABLE OaspJoinWork (work_id LONG, asset_id LONG)"
connection.Execute "CREATE TABLE OaspJoinAsset (asset_id LONG, class_id LONG)"
connection.Execute "CREATE TABLE OaspJoinClass (class_id LONG, class_name TEXT(20))"
connection.Execute "INSERT INTO OaspJoinWork (work_id, asset_id) VALUES (1, 10)"
connection.Execute "INSERT INTO OaspJoinAsset (asset_id, class_id) VALUES (10, 7)"
connection.Execute "INSERT INTO OaspJoinClass (class_id, class_name) VALUES (7, 'motor')"
Set recordset = connection.Execute("SELECT OaspJoinClass.class_name FROM (OaspJoinWork INNER JOIN OaspJoinAsset ON OaspJoinWork.asset_id=OaspJoinAsset.asset_id) INNER JOIN OaspJoinClass ON OaspJoinAsset.class_id=OaspJoinClass.class_id")
Response.Write recordset(0)
connection.Execute "DROP TABLE OaspJoinClass"
connection.Execute "DROP TABLE OaspJoinAsset"
connection.Execute "DROP TABLE OaspJoinWork"
%>

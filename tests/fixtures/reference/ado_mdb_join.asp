<%
Dim connection, recordset
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db"))
Set recordset = connection.Execute("select tblLabel.sCode, tblLabelValue.sValue from tblLabelValue INNER JOIN tblLabel on tblLabel.iId=tblLabelValue.iLabelId where tblLabelValue.iLanguageID=1")
Response.Write(recordset.RecordCount & ":" & recordset("sCode") & ":" & recordset("sValue"))
%>

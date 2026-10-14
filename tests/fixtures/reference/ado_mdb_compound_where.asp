<%
Dim connection, rows, count
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db"))
Set rows = connection.Execute("select iId,iParentID from tblPage where iListPageID is null and iCustomerID=73 and bLossePagina=False and bDeleted=False and iParentID=492 and bOnline=True order by iRang asc")
count = 0
Do While Not rows.EOF And count < 20
    Response.Write rows("iId") & ":" & rows("iParentID") & ";"
    count = count + 1
    Call rows.MoveNext()
Loop
%>

<%
Class SourceRow
    Public ID
    Public Title

    Public Function Load(values)
        ID = values(0)
        Title = values(1)
        Load = True
    End Function
End Class

Dim connection, rows
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=Microsoft.Jet.OLEDB.4.0;Data Source=" & Request("db"))

Set rows = Server.CreateObject("ADODB.Recordset")
Set rows.ActiveConnection = connection
rows.Source = Request("sql")
Call rows.Open()

Response.Write "source=" & rows.Source
Response.Write ";count=" & rows.RecordCount
Response.Write ";bof=" & rows.BOF
Response.Write ";eof=" & rows.EOF
If Not rows.EOF Then
    Dim firstRow
    firstRow = Array(rows(0), rows(1), rows(2))
    Response.Write ";first=" & firstRow(0)
    Response.Write ";third=" & firstRow(2)
    Response.Write ";isArray=" & IsArray(firstRow)
    Response.Write ";upper=" & UBound(firstRow)

    Dim rowObject, rowMap
    Set rowObject = New SourceRow
    Set rowMap = Server.CreateObject("Scripting.Dictionary")
    If rowObject.Load(Array(rows(0), rows(1))) Then
        rowMap.Add CLng(rows(0)), rowObject
    End If
    Response.Write ";objectId=" & rowObject.ID
    Response.Write ";objectTitle=" & rowObject.Title
    Response.Write ";mapCount=" & rowMap.Count
End If
%>

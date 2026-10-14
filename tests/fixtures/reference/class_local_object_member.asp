<%
Dim connection
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=SQLite;Data Source=:memory:")
Call connection.Execute("drop table if exists article; create table article(id integer primary key, title text); insert into article values(1, 'local-slot')")

Class ArticleProbe
    Public ID
    Public Title

    Public Function LoadByID(articleID)
        Dim rows
        Set rows = connection.Execute("select id, title from article where id=" & articleID)
        If (Not rows.BOF) And (Not rows.EOF) Then
            ID = rows("id")
            Title = rows("title")
            LoadByID = True
        End If
    End Function
End Class

Dim article
Set article = New ArticleProbe
Response.Write CStr(article.LoadByID(1)) & "|" & article.ID & "|" & article.Title
%>

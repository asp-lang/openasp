<!--#include file="bootstrap.inc"-->
<%
Dim database, pageNumber, offset, rows, content
Set database = BlogDb()
pageNumber = 1
If IsNumeric(Request.QueryString("page")) Then pageNumber = CInt(Request.QueryString("page"))
If pageNumber < 1 Then pageNumber = 1
offset = (pageNumber - 1) * PostsPerPage
Set rows = database.Execute("SELECT id,title,slug,summary,body,published_at,views FROM posts " & _
                            "ORDER BY published_at DESC,id DESC LIMIT " & CStr(PostsPerPage) & _
                            " OFFSET " & CStr(offset))
content = "<h2>Latest articles</h2>"
Do While Not rows.EOF
    content = content & BlogRenderPost(database, rows, False)
    rows.MoveNext
Loop
rows.Close
Call BlogRenderPage("Home", content, database)
database.Close
%>

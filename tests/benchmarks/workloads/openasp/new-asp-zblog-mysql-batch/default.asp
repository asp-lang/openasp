<!--#include file="bootstrap.inc"-->
<%
Dim database, pageNumber, offset, rows, content
Set database = BlogDb()
pageNumber = 1
If IsNumeric(Request.QueryString("page")) Then pageNumber = CInt(Request.QueryString("page"))
If pageNumber < 1 Then pageNumber = 1
offset = (pageNumber - 1) * PostsPerPage
Set rows = database.Execute("SELECT p.id,p.title,p.slug,p.summary,p.body,p.published_at,p.views," & _
                            "COALESCE(GROUP_CONCAT(t.name ORDER BY t.name SEPARATOR ','),'') AS tag_names " & _
                            "FROM posts p LEFT JOIN post_tags pt ON pt.post_id=p.id " & _
                            "LEFT JOIN tags t ON t.id=pt.tag_id " & _
                            "GROUP BY p.id,p.title,p.slug,p.summary,p.body,p.published_at,p.views " & _
                            "ORDER BY p.published_at DESC,p.id DESC LIMIT " & CStr(PostsPerPage) & _
                            " OFFSET " & CStr(offset))
content = "<h2>Latest articles</h2>"
Do While Not rows.EOF
    content = content & BlogRenderPost(rows, False)
    rows.MoveNext
Loop
rows.Close
Call BlogRenderPage("Home", content, database)
database.Close
%>

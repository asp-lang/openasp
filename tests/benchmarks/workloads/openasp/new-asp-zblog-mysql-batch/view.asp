<!--#include file="bootstrap.inc"-->
<%
Dim database, postId, rows, content
Set database = BlogDb()
postId = 1
If IsNumeric(Request.QueryString("id")) Then postId = CInt(Request.QueryString("id"))
Set rows = database.Execute("SELECT p.id,p.title,p.slug,p.summary,p.body,p.published_at,p.views," & _
                            "COALESCE(GROUP_CONCAT(t.name ORDER BY t.name SEPARATOR ','),'') AS tag_names " & _
                            "FROM posts p LEFT JOIN post_tags pt ON pt.post_id=p.id " & _
                            "LEFT JOIN tags t ON t.id=pt.tag_id WHERE p.id=" & CStr(postId) & _
                            " GROUP BY p.id,p.title,p.slug,p.summary,p.body,p.published_at,p.views")
If rows.EOF Then
    Response.Status = "404 Not Found"
    content = "<h2>Article not found</h2>"
Else
    content = BlogRenderPost(rows, True)
End If
rows.Close
Call BlogRenderPage("Article", content, database)
database.Close
%>

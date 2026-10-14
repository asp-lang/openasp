<!--#include file="bootstrap.inc"-->
<%
Dim database, postId, rows, content
Set database = BlogDb()
postId = 1
If IsNumeric(Request.QueryString("id")) Then postId = CInt(Request.QueryString("id"))
Set rows = database.Execute("SELECT id,title,slug,summary,body,published_at,views FROM posts WHERE id=" & CStr(postId))
If rows.EOF Then
    Response.Status = "404 Not Found"
    content = "<h2>Article not found</h2>"
Else
    content = BlogRenderPost(database, rows, True)
End If
rows.Close
Call BlogRenderPage("Article", content, database)
database.Close
%>

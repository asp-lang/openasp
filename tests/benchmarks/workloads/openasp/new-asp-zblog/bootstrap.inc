<%
Const BlogName = "OpenASP Benchmark Blog"
Const PostsPerPage = 10

Function BlogDb()
    Dim database
    Set database = Server.CreateObject("ADODB.Connection")
    database.Open "Provider=SQLite;Data Source=" & Server.MapPath("blog.sqlite")
    Set BlogDb = database
End Function

Function BlogEscape(value)
    BlogEscape = Server.HTMLEncode(CStr(value))
End Function

Function BlogSqlString(value)
    BlogSqlString = "'" & Replace(CStr(value), "'", "''") & "'"
End Function

Function BlogPostTags(database, postId)
    Dim rows, output
    output = ""
    Set rows = database.Execute("SELECT t.name FROM tags t JOIN post_tags pt ON pt.tag_id=t.id " & _
                                "WHERE pt.post_id=" & CStr(postId) & " ORDER BY t.name")
    Do While Not rows.EOF
        If Len(output) > 0 Then output = output & ", "
        output = output & "<a href=""/tags.asp?tag=" & Server.URLEncode(rows.Fields("name")) & """>" & _
                 BlogEscape(rows.Fields("name")) & "</a>"
        rows.MoveNext
    Loop
    rows.Close
    BlogPostTags = output
End Function

Function BlogRenderPost(database, post, fullPost)
    Dim text, body
    If fullPost Then
        text = CStr(post.Fields("body"))
    Else
        text = CStr(post.Fields("summary"))
    End If
    body = "<p>" & Replace(BlogEscape(text), vbLf & vbLf, "</p><p>") & "</p>"
    BlogRenderPost = "<article><header><h2><a href=""/view.asp?id=" & CStr(post.Fields("id")) & """>" & _
                     BlogEscape(post.Fields("title")) & "</a></h2><time>" & _
                     BlogEscape(post.Fields("published_at")) & "</time></header>" & body & _
                     "<footer><span>" & BlogPostTags(database, post.Fields("id")) & "</span><span>" & _
                     CStr(post.Fields("views")) & " views</span></footer></article>"
End Function

Sub BlogRenderPage(title, content, database)
    Dim visitor, tags, archives
    visitor = Session("visitor")
    If IsEmpty(visitor) Then visitor = "guest"
    Set tags = database.Execute("SELECT t.name, COUNT(pt.post_id) AS post_count FROM tags t " & _
                                "JOIN post_tags pt ON pt.tag_id=t.id GROUP BY t.id,t.name " & _
                                "ORDER BY post_count DESC,t.name")
    Set archives = database.Execute("SELECT substr(published_at,1,7) AS month,COUNT(*) AS post_count " & _
                                    "FROM posts GROUP BY month ORDER BY month DESC LIMIT 12")
    Response.ContentType = "text/html; charset=utf-8"
    Response.Write "<!doctype html><html lang=""en""><head><meta charset=""utf-8""><meta name=""viewport"" content=""width=device-width,initial-scale=1"">"
    Response.Write "<title>" & BlogEscape(title) & " - " & BlogName & "</title>"
    Response.Write "<style>body{font:15px system-ui;margin:0;color:#242424;background:#f4f5f7}header,main,footer{max-width:1060px;margin:auto}"
    Response.Write "body>header{padding:24px 20px;background:#fff;border-bottom:1px solid #ddd}nav a{margin-right:18px;color:#185ea8}"
    Response.Write "main{display:grid;grid-template-columns:minmax(0,1fr) 260px;gap:24px;padding:24px 20px}article{background:#fff;padding:18px;margin-bottom:16px}"
    Response.Write "article h2{margin:0 0 8px;font-size:21px}article footer{display:flex;justify-content:space-between;color:#666}"
    Response.Write "aside{background:#fff;padding:18px;align-self:start}aside ul{padding-left:18px}a{color:#185ea8;text-decoration:none}"
    Response.Write "body>footer{padding:20px;color:#666}@media(max-width:720px){main{grid-template-columns:1fr}}</style></head><body>"
    Response.Write "<header><h1>" & BlogName & "</h1><nav><a href=""/default.asp"">Home</a><a href=""/search.asp"">Search</a>"
    Response.Write "<a href=""/tags.asp"">Tags</a><a href=""/feed.asp"">Feed</a></nav></header><main><section>" & content & "</section><aside>"
    Response.Write "<h3>Popular tags</h3><ul>"
    Do While Not tags.EOF
        Response.Write "<li>" & BlogEscape(tags.Fields("name")) & " (" & CStr(tags.Fields("post_count")) & ")</li>"
        tags.MoveNext
    Loop
    tags.Close
    Response.Write "</ul><h3>Archives</h3><ul>"
    Do While Not archives.EOF
        Response.Write "<li>" & BlogEscape(archives.Fields("month")) & " (" & CStr(archives.Fields("post_count")) & ")</li>"
        archives.MoveNext
    Loop
    archives.Close
    Response.Write "</ul></aside></main><footer>Visitor: " & BlogEscape(visitor) & _
                   " | Powered by OpenASP-C</footer></body></html>"
End Sub
%>

<!--#include file="bootstrap.inc"-->
<%
Dim database, query, rows, content, count
Set database = BlogDb()
query = Trim(Request.QueryString("q"))
If Len(query) = 0 Then query = "runtime"
Set rows = database.Execute("SELECT p.id,p.title,p.slug,p.summary,p.body,p.published_at,p.views," & _
                            "COALESCE(GROUP_CONCAT(t.name ORDER BY t.name SEPARATOR ','),'') AS tag_names " & _
                            "FROM posts p LEFT JOIN post_tags pt ON pt.post_id=p.id " & _
                            "LEFT JOIN tags t ON t.id=pt.tag_id " & _
                            "WHERE p.title LIKE " & BlogSqlString("%" & query & "%") & _
                            " OR p.summary LIKE " & BlogSqlString("%" & query & "%") & _
                            " OR p.body LIKE " & BlogSqlString("%" & query & "%") & _
                            " GROUP BY p.id,p.title,p.slug,p.summary,p.body,p.published_at,p.views " & _
                            "ORDER BY p.published_at DESC LIMIT 20")
content = ""
count = 0
Do While Not rows.EOF
    content = content & BlogRenderPost(rows, False)
    count = count + 1
    rows.MoveNext
Loop
rows.Close
content = "<h2>Search: " & BlogEscape(query) & "</h2><p>" & CStr(count) & _
          " matching articles.</p>" & content
Call BlogRenderPage("Search", content, database)
database.Close
%>

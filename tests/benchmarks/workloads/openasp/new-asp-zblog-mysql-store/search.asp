<!--#include file="bootstrap.inc"-->
<%
Dim database, query, rows, content, count
Set database = BlogDb()
query = Trim(Request.QueryString("q"))
If Len(query) = 0 Then query = "runtime"
Set rows = database.Execute("SELECT id,title,slug,summary,body,published_at,views FROM posts " & _
                            "WHERE title LIKE " & BlogSqlString("%" & query & "%") & _
                            " OR summary LIKE " & BlogSqlString("%" & query & "%") & _
                            " OR body LIKE " & BlogSqlString("%" & query & "%") & _
                            " ORDER BY published_at DESC LIMIT 20")
content = ""
count = 0
Do While Not rows.EOF
    content = content & BlogRenderPost(database, rows, False)
    count = count + 1
    rows.MoveNext
Loop
rows.Close
content = "<h2>Search: " & BlogEscape(query) & "</h2><p>" & CStr(count) & _
          " matching articles.</p>" & content
Call BlogRenderPage("Search", content, database)
database.Close
%>

<!--#include file="bootstrap.inc"-->
<%
Dim database, selected, tags, rows, content, size, name
Set database = BlogDb()
selected = Trim(Request.QueryString("tag"))
content = "<h2>Tag cloud</h2><div>"
Set tags = database.Execute("SELECT t.name,COUNT(pt.post_id) AS post_count FROM tags t " & _
                            "JOIN post_tags pt ON pt.tag_id=t.id GROUP BY t.id,t.name " & _
                            "ORDER BY post_count DESC,t.name")
Do While Not tags.EOF
    size = 13 + tags.Fields("post_count")
    If size > 29 Then size = 29
    name = CStr(tags.Fields("name"))
    content = content & "<a style=""font-size:" & CStr(size) & _
              "px;margin-right:14px"" href=""/tags.asp?tag=" & Server.URLEncode(name) & """>" & _
              BlogEscape(name) & "</a>"
    tags.MoveNext
Loop
tags.Close
content = content & "</div>"
If Len(selected) > 0 Then
    Set rows = database.Execute("SELECT p.id,p.title,p.slug,p.summary,p.body,p.published_at,p.views " & _
                                "FROM posts p JOIN post_tags pt ON pt.post_id=p.id " & _
                                "JOIN tags t ON t.id=pt.tag_id WHERE t.name=" & BlogSqlString(selected) & _
                                " ORDER BY p.published_at DESC LIMIT 20")
    Do While Not rows.EOF
        content = content & BlogRenderPost(database, rows, False)
        rows.MoveNext
    Loop
    rows.Close
End If
Call BlogRenderPage("Tags", content, database)
database.Close
%>

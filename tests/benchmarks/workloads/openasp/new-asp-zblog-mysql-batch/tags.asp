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
    Set rows = database.Execute("SELECT p.id,p.title,p.slug,p.summary,p.body,p.published_at,p.views," & _
                                "COALESCE(GROUP_CONCAT(all_tags.name ORDER BY all_tags.name SEPARATOR ','),'') AS tag_names " & _
                                "FROM posts p JOIN post_tags selected_tags ON selected_tags.post_id=p.id " & _
                                "JOIN tags selected_tag ON selected_tag.id=selected_tags.tag_id " & _
                                "LEFT JOIN post_tags all_post_tags ON all_post_tags.post_id=p.id " & _
                                "LEFT JOIN tags all_tags ON all_tags.id=all_post_tags.tag_id " & _
                                "WHERE selected_tag.name=" & BlogSqlString(selected) & _
                                " GROUP BY p.id,p.title,p.slug,p.summary,p.body,p.published_at,p.views " & _
                                "ORDER BY p.published_at DESC LIMIT 20")
    Do While Not rows.EOF
        content = content & BlogRenderPost(rows, False)
        rows.MoveNext
    Loop
    rows.Close
End If
Call BlogRenderPage("Tags", content, database)
database.Close
%>

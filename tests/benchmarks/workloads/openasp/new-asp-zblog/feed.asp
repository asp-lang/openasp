<!--#include file="bootstrap.inc"-->
<%
Dim database, posts
Set database = BlogDb()
Set posts = database.Execute("SELECT id,title,slug,summary,published_at FROM posts " & _
                             "ORDER BY published_at DESC,id DESC LIMIT 20")
Response.ContentType = "application/rss+xml; charset=utf-8"
Response.Write "<?xml version=""1.0"" encoding=""UTF-8""?>"
Response.Write "<rss version=""2.0""><channel><title>" & BlogName & "</title>"
Response.Write "<link>http://localhost/default.asp</link><description>Recent articles</description>"
Do While Not posts.EOF
    Response.Write "<item><title>" & BlogEscape(posts.Fields("title")) & "</title>"
    Response.Write "<link>http://localhost/view.asp?id=" & CStr(posts.Fields("id")) & "</link>"
    Response.Write "<guid>benchmark-blog-" & CStr(posts.Fields("id")) & "</guid>"
    Response.Write "<pubDate>" & BlogEscape(posts.Fields("published_at")) & "</pubDate>"
    Response.Write "<description>" & BlogEscape(posts.Fields("summary")) & "</description></item>"
    posts.MoveNext
Loop
posts.Close
Response.Write "</channel></rss>"
database.Close
%>

<!--#include file="bootstrap.inc"-->
<%
Dim redis, values, value, post
Set redis = BlogRedis()
values = BlogLoadPosts(redis, "zblog:posts", 0, 19)
Response.ContentType = "application/rss+xml; charset=utf-8"
Response.Write "<?xml version=""1.0"" encoding=""UTF-8""?>"
Response.Write "<rss version=""2.0""><channel><title>" & BlogName & "</title>"
Response.Write "<link>http://localhost/default.asp</link><description>Recent articles</description>"
For Each value In values
    post = BlogPost(value)
    Response.Write "<item><title>" & BlogEscape(post(1)) & "</title>"
    Response.Write "<link>http://localhost/view.asp?id=" & CStr(post(0)) & "</link>"
    Response.Write "<guid>benchmark-blog-" & CStr(post(0)) & "</guid>"
    Response.Write "<pubDate>" & BlogEscape(post(5)) & "</pubDate>"
    Response.Write "<description>" & BlogEscape(post(3)) & "</description></item>"
Next
Response.Write "</channel></rss>"
Call redis.Close()
%>

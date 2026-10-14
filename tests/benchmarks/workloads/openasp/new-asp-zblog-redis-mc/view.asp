<!--#include file="bootstrap.inc"-->
<%
Dim redis, cache, postId, value, content, post, postTags
Set redis = BlogRedis()
Set cache = BlogMemcache()
postId = 1
If IsNumeric(Request.QueryString("id")) Then postId = CInt(Request.QueryString("id"))
value = redis.Get("zblog:post:" & CStr(postId))
If IsNull(value) Then
    Response.Status = "404 Not Found"
    content = "<h2>Article not found</h2>"
Else
    post = BlogPost(value)
    Set postTags = cache.GetMulti(Array("zblog:posttags:" & CStr(post(0))))
    content = BlogRenderPost(postTags, post, True)
End If
Call BlogRenderPage("Article", content, cache)
Call redis.Close()
Call cache.Close()
%>

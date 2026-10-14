<!--#include file="bootstrap.inc"-->
<%
Dim redis, cache, pageNumber, offset, values, value, content, post, postTags
Set redis = BlogRedis()
Set cache = BlogMemcache()
pageNumber = 1
If IsNumeric(Request.QueryString("page")) Then pageNumber = CInt(Request.QueryString("page"))
If pageNumber < 1 Then pageNumber = 1
offset = (pageNumber - 1) * PostsPerPage
values = BlogLoadPosts(redis, "zblog:posts", offset, offset + PostsPerPage - 1)
Set postTags = BlogLoadPostTags(cache, values)
content = "<h2>Latest articles</h2>"
For Each value In values
    post = BlogPost(value)
    content = content & BlogRenderPost(postTags, post, False)
Next
Call BlogRenderPage("Home", content, cache)
Call redis.Close()
Call cache.Close()
%>

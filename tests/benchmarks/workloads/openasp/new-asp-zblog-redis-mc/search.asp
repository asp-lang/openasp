<!--#include file="bootstrap.inc"-->
<%
Dim redis, cache, query, values, value, content, count, post, postTags
Set redis = BlogRedis()
Set cache = BlogMemcache()
query = LCase(Trim(Request.QueryString("q")))
If Len(query) = 0 Then query = "runtime"
values = BlogLoadPosts(redis, "zblog:search:" & query, 0, 19)
Set postTags = BlogLoadPostTags(cache, values)
content = ""
count = 0
For Each value In values
    post = BlogPost(value)
    content = content & BlogRenderPost(postTags, post, False)
    count = count + 1
Next
content = "<h2>Search: " & BlogEscape(query) & "</h2><p>" & CStr(count) & _
          " matching articles.</p>" & content
Call BlogRenderPage("Search", content, cache)
Call redis.Close()
Call cache.Close()
%>

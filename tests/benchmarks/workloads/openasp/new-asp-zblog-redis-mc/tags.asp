<!--#include file="bootstrap.inc"-->
<%
Dim redis, cache, selected, values, value, content, post, postTags
Set redis = BlogRedis()
Set cache = BlogMemcache()
selected = LCase(Trim(Request.QueryString("tag")))
content = "<h2>Tag cloud</h2><div>" & BlogTagCloud(cache) & "</div>"
If Len(selected) > 0 Then
    values = BlogLoadPosts(redis, "zblog:tag:" & selected, 0, 19)
    Set postTags = BlogLoadPostTags(cache, values)
    For Each value In values
        post = BlogPost(value)
        content = content & BlogRenderPost(postTags, post, False)
    Next
End If
Call BlogRenderPage("Tags", content, cache)
Call redis.Close()
Call cache.Close()
%>

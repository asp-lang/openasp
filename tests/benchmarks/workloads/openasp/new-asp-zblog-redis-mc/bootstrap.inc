<%
Const BlogName = "OpenASP Benchmark Blog"
Const PostsPerPage = 10
Const BlogRedisPort = 16379
Const BlogMemcachePort = 19146

Function BlogRedis()
    Dim client
    Set client = Server.CreateObject("OpenASP.Redis")
    Call client.Open("Host=127.0.0.1;Port=" & CStr(BlogRedisPort) & ";Database=0;Timeout=2000")
    Set BlogRedis = client
End Function

Function BlogMemcache()
    Dim cache
    Set cache = Server.CreateObject("OpenASP.Memcache")
    cache.Host = "127.0.0.1"
    cache.Port = BlogMemcachePort
    cache.Timeout = 2000
    cache.Protocol = "text"
    Call cache.Open()
    Set BlogMemcache = cache
End Function

Function BlogEscape(value)
    BlogEscape = Server.HTMLEncode(CStr(value))
End Function

Function BlogPost(value)
    BlogPost = Split(CStr(value), Chr(9))
End Function

Function BlogLoadPosts(redis, indexKey, first, last)
    Dim ids, keys, index
    ids = redis.LRange(indexKey, first, last)
    If IsArray(ids) Then
        ReDim keys(UBound(ids))
        For index = 0 To UBound(ids)
            keys(index) = "zblog:post:" & CStr(ids(index))
        Next
        BlogLoadPosts = redis.Execute("MGET", keys)
    Else
        BlogLoadPosts = Array()
    End If
End Function

Function BlogLoadPostTags(cache, values)
    Dim keys, index, post, emptyTags
    If IsArray(values) Then
        ReDim keys(UBound(values))
        For index = 0 To UBound(values)
            post = BlogPost(values(index))
            keys(index) = "zblog:posttags:" & CStr(post(0))
        Next
        Set BlogLoadPostTags = cache.GetMulti(keys)
    Else
        Set emptyTags = Server.CreateObject("Scripting.Dictionary")
        Set BlogLoadPostTags = emptyTags
    End If
End Function

Function BlogPostTags(tags, postId)
    Dim key, encoded, names, name, output
    key = "zblog:posttags:" & CStr(postId)
    If tags.Exists(key) Then
        encoded = tags(key)
    Else
        encoded = Null
    End If
    output = ""
    If Not IsNull(encoded) Then
        names = Split(CStr(encoded), ",")
        For Each name In names
            If Len(output) > 0 Then output = output & ", "
            output = output & "<a href=""/tags.asp?tag=" & Server.URLEncode(name) & """>" & _
                     BlogEscape(name) & "</a>"
        Next
    End If
    BlogPostTags = output
End Function

Function BlogRenderPost(tags, post, fullPost)
    Dim text, body
    If fullPost Then
        text = CStr(post(4))
    Else
        text = CStr(post(3))
    End If
    body = "<p>" & Replace(BlogEscape(text), vbLf & vbLf, "</p><p>") & "</p>"
    BlogRenderPost = "<article><header><h2><a href=""/view.asp?id=" & CStr(post(0)) & """>" & _
                     BlogEscape(post(1)) & "</a></h2><time>" & BlogEscape(post(5)) & _
                     "</time></header>" & body & "<footer><span>" & _
                     BlogPostTags(tags, post(0)) & "</span><span>" & CStr(post(6)) & _
                     " views</span></footer></article>"
End Function

Function BlogMetadataList(cache, key)
    Dim encoded, entries, entry, fields, output
    encoded = cache.Get(key)
    output = ""
    If Not IsNull(encoded) Then
        entries = Split(CStr(encoded), vbLf)
        For Each entry In entries
            If Len(entry) > 0 Then
                fields = Split(entry, Chr(9))
                output = output & "<li>" & BlogEscape(fields(0)) & " (" & CStr(fields(1)) & ")</li>"
            End If
        Next
    End If
    BlogMetadataList = output
End Function

Function BlogTagCloud(cache)
    Dim encoded, entries, entry, fields, output, size
    encoded = cache.Get("zblog:tags")
    output = ""
    If Not IsNull(encoded) Then
        entries = Split(CStr(encoded), vbLf)
        For Each entry In entries
            If Len(entry) > 0 Then
                fields = Split(entry, Chr(9))
                size = 13 + CInt(fields(1))
                If size > 29 Then size = 29
                output = output & "<a style=""font-size:" & CStr(size) & _
                         "px;margin-right:14px"" href=""/tags.asp?tag=" & _
                         Server.URLEncode(fields(0)) & """>" & BlogEscape(fields(0)) & "</a>"
            End If
        Next
    End If
    BlogTagCloud = output
End Function

Sub BlogRenderPage(title, content, cache)
    Dim visitor
    visitor = Session("visitor")
    If IsEmpty(visitor) Then visitor = "guest"
    Response.ContentType = "text/html; charset=utf-8"
    Response.Write "<!doctype html><html lang=""en""><head><meta charset=""utf-8""><meta name=""viewport"" content=""width=device-width,initial-scale=1"">"
    Response.Write "<title>" & BlogEscape(title) & " - " & BlogName & "</title>"
    Response.Write "<style>body{font:15px system-ui;margin:0;color:#242424;background:#f4f5f7}header,main,footer{max-width:1060px;margin:auto}"
    Response.Write "body>header{padding:24px 20px;background:#fff;border-bottom:1px solid #ddd}nav a{margin-right:18px;color:#185ea8}"
    Response.Write "main{display:grid;grid-template-columns:minmax(0,1fr) 260px;gap:24px;padding:24px 20px}article{background:#fff;padding:18px;margin-bottom:16px}"
    Response.Write "article h2{margin:0 0 8px;font-size:21px}article footer{display:flex;justify-content:space-between;color:#666}"
    Response.Write "aside{background:#fff;padding:18px;align-self:start}aside ul{padding-left:18px}a{color:#185ea8;text-decoration:none}"
    Response.Write "body>footer{padding:20px;color:#666}@media(max-width:720px){main{grid-template-columns:1fr}}</style></head><body>"
    Response.Write "<header><h1>" & BlogName & "</h1><nav><a href=""/default.asp"">Home</a><a href=""/search.asp"">Search</a>"
    Response.Write "<a href=""/tags.asp"">Tags</a><a href=""/feed.asp"">Feed</a></nav></header><main><section>" & content & "</section><aside>"
    Response.Write "<h3>Popular tags</h3><ul>" & BlogMetadataList(cache, "zblog:tags") & _
                   "</ul><h3>Archives</h3><ul>" & BlogMetadataList(cache, "zblog:archives") & _
                   "</ul></aside></main><footer>Visitor: " & BlogEscape(visitor) & _
                   " | Powered by OpenASP-C</footer></body></html>"
End Sub
%>

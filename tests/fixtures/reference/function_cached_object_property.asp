<%
Class CachedChild
    Public ConnectionString
End Class

Dim cached
cached = Null
Dim outside
Set outside = New CachedChild
outside.ConnectionString = 99
Response.Write "outside=" & outside.ConnectionString & ";"

Function GetCached()
    If IsNull(cached) Then
        Set cached = New CachedChild
        cached.ConnectionString = 413
        Response.Write "inside=" & cached.ConnectionString & ";"
    End If
    Set GetCached = cached
End Function

Dim direct
Set direct = GetCached
Response.Write IsObject(GetCached) & ":" & direct.ConnectionString & ":" & GetCached.ConnectionString
%>

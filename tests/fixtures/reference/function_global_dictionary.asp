<%
Dim cache

Function CachedValue(key)
    If Not IsObject(cache) Then Set cache = CreateObject("Scripting.Dictionary")
    If cache.Exists(key) Then
        CachedValue = cache(key)
        Exit Function
    End If
    CachedValue = "value-" & key
    cache(key) = CachedValue
End Function

Response.Write CachedValue("a") & ":" & CachedValue("a") & ":" & IsObject(cache) & ":" & cache.Count
%>

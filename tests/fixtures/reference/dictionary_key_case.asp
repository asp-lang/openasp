<%
Dim dictionary, keys
Set dictionary = CreateObject("Scripting.Dictionary")
dictionary.Add "BlogTitle", "first"
dictionary.Item("blogtitle") = "updated"
dictionary.Add "ZC_BLOG_HOST", "/"

keys = dictionary.Keys
Response.Write keys(0) & "|" & keys(1) & "|" & dictionary.Item("BLOGTITLE")
%>

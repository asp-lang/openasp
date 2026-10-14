<%
Dim dictionary, outerKey, innerKey, keys, values, text
Set dictionary = Server.CreateObject("Scripting.Dictionary")
dictionary.Add "TEMPLATE_HEADER", "header"
dictionary.Add "TEMPLATE_DEFAULT", "<#TEMPLATE_HEADER#>|<#ZC_MSG011#>"
dictionary.Add "ZC_MSG011", "publish"

For Each outerKey In dictionary.Keys
    For Each innerKey In dictionary.Keys
        dictionary.Item(innerKey) = Replace(dictionary.Item(innerKey), "<#" & outerKey & "#>", dictionary.Item(outerKey))
    Next
Next

keys = dictionary.Keys
values = dictionary.Items
For Each outerKey In keys
    Response.Write outerKey & "=" & dictionary.Item(outerKey) & ";"
Next
Response.Write "|"
Response.Write keys(0) & "=" & values(0) & ";"
Response.Write keys(1) & "=" & values(1) & ";"
Response.Write keys(2) & "=" & values(2)
%>

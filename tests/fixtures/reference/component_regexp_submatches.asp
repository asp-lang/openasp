<%
Dim expression, matches, captures, capture, joined
Set expression = Server.CreateObject("VBScript.RegExp")
expression.Pattern = "(article\-([a-z0-9]*)([\-a-z0-9]*))"
expression.IgnoreCase = True
Set matches = expression.Execute("<#template:article-multi#>")
Set captures = matches(0).SubMatches

Response.Write matches.Count & "|"
Response.Write matches(0).Value & "|"
Response.Write matches(0).SubMatches(0) & "|"
Response.Write matches(0).SubMatches(1) & "|"
Response.Write matches(0).SubMatches(2) & "|"
Response.Write captures.Count & "|"
Response.Write captures(0) & "|"

joined = ""
For Each capture In captures
    If Len(joined) > 0 Then joined = joined & ","
    joined = joined & capture
Next
Response.Write joined
%>

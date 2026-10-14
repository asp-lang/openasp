<%
Set first = Server.CreateObject("Scripting.Dictionary")
Set same = first
Set other = Server.CreateObject("Scripting.Dictionary")
Set missing = Nothing
If first Is same Then Response.Write "same|" : End If
If Not first Is other Then Response.Write "different|" : End If
If missing Is Nothing Then Response.Write "nothing|" : End If
If Not first Is Nothing Then Response.Write "object" : End If
%>

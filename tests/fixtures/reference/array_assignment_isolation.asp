<%
Dim original, copied
original = Array("zero", "one")
copied = original
copied(0) = "changed"
Response.Write original(0) & ":" & copied(0)
%>

<%
Dim unsetValue
Response.Write IsNull(Null)
Response.Write ":"
Response.Write IsNull(unsetValue)
Response.Write ":"
Response.Write IsEmpty(unsetValue)
Response.Write ":"
Response.Write IsEmpty(Null)
Response.Write ":"
Response.Write "[" & Trim("  value  ") & "]"
Response.Write ":"
Response.Write IsNull(Trim(Null))
%>

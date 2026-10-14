<%
Dim username, userpwd
username = "testuser"
userpwd = "test123"
Response.Write Len(username)
Response.Write "|"
Response.Write Len("")
Response.Write "|"
Response.Write username="" Or userpwd="" Or Len(username)<3 Or Len(userpwd)<3
Response.Write "|"
Response.Write True And 2
Response.Write "|"
Response.Write False Or 4
%>

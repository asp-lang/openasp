<%
Function Hoor()
    Hoor = "ok"
End Function

Response.Write ChrB(&H41)
Response.Write "|" & Hex(&H2A)
Response.Write "|"&Hoor()
%>

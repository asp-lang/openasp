<%
Response.Write Request.Cookies("ACTCMS")("UserName") & ":" & _
               Request.Cookies("ACTCMS")("PassWord") & ":" & _
               Request.Cookies("ACTCMS")("missing")
%>

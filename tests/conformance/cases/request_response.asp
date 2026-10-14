<%
Response.Status = "201 Created"
Response.ContentType = "text/plain"
Response.Charset = "utf-8"
Response.AddHeader "X-Conformance", "ok"
Response.Cookies("mode") = "tested"
Response.Cookies("mode").Path = "/"
Response.Write Request.QueryString("name") & "|" & Request.Form("value") & "|" & Request.ServerVariables("request_method")
%>

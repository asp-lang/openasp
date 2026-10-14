<%
Set dict = Server.CreateObject("Scripting.Dictionary")
dict("alpha") = "one"
Call dict.Add("beta", "two")
Set dropped = dict
Set dropped = Nothing

Response.Write dict("alpha")
Response.Write ":"
Response.Write CStr(dict.Exists("beta"))
Response.Write ":"
Response.Write CStr(IsObject(dropped))
Response.Write ":"
Response.Write Request.ServerVariables("request_method")
Response.BinaryWrite ":" & Request.BinaryRead(Request.TotalBytes)
%>

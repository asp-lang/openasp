<%
Set stream = Server.CreateObject("ADODB.Stream")
Response.Write stream.Type
Response.Write ":"
Call stream.Open()
Call stream.WriteText("hello")
stream.Position = 0
Response.Write stream.ReadText()
%>
